/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "cros_gralloc_driver.h"

#include <cstdlib>
#include <cutils/properties.h>
#include <fcntl.h>
#include <hardware/gralloc.h>
#include <sys/mman.h>
#include <syscall.h>
#include <xf86drm.h>

#include "../drv_priv.h"
#include "../intel_device.h"
#include "../util.h"
#include "drv.h"

// Constants taken from pipe_loader_drm.c in Mesa

#define DRM_NUM_NODES 8

// DRM Render nodes start at 128
#define DRM_RENDER_NODE_START 128

// DRM Card nodes start at 0
#define DRM_CARD_NODE_START 0

#define IVSH_WIDTH 1600
#define IVSH_HEIGHT 900

#define IVSH_DEVICE_NUM 2

class cros_gralloc_driver_preloader
{
      public:
	cros_gralloc_driver_preloader()
	{
		drv_preload(true);
	}

	~cros_gralloc_driver_preloader()
	{
		drv_preload(false);
	}
};

static class cros_gralloc_driver_preloader cros_gralloc_driver_preloader;

int memfd_create_wrapper(const char *name, unsigned int flags)
{
	int fd;

#if defined(HAVE_MEMFD_CREATE)
	fd = memfd_create(name, flags);
#elif defined(__NR_memfd_create)
	fd = syscall(__NR_memfd_create, name, flags);
#else
	ALOGE("Failed to create memfd '%s': memfd_create not available.", name);
	return -1;
#endif

	if (fd == -1)
		ALOGE("Failed to create memfd '%s': %s.", name, strerror(errno));

	return fd;
}

int memfd_create_reserved_region(const std::string &buffer_name, uint64_t reserved_region_size)
{
	const std::string reserved_region_name = buffer_name + " reserved region";

	int reserved_region_fd = memfd_create_wrapper(reserved_region_name.c_str(), FD_CLOEXEC);
	if (reserved_region_fd == -1)
		return -errno;

	if (ftruncate(reserved_region_fd, reserved_region_size)) {
		ALOGE("Failed to set reserved region size: %s.", strerror(errno));
		return -errno;
	}

	return reserved_region_fd;
}

cros_gralloc_driver *cros_gralloc_driver::get_instance()
{
	static cros_gralloc_driver s_instance;

	if (!s_instance.is_initialized()) {
		ALOGE("Failed to initialize driver.");
		return nullptr;
	}

	return &s_instance;
}

#define DRV_DESTROY(drv)    \
    if (drv) {                       \
        int fd = drv_get_fd(drv);    \
        drv_destroy(drv);            \
        drv = nullptr;               \
        close(fd);                   \
    }

int32_t cros_gralloc_driver::reload()
{
	int fd;
	char const *str = "%s/renderD%d";
	char node[64];

	if (gpu_grp_type_ & GPU_GRP_TYPE_HAS_VIRTIO_GPU_IVSHMEM_BIT) {
		return 0;
	}
	// Max probe two ivshm node, the first one is used for screen cast.
	for (int i = 0; i < DRM_NUM_NODES; ++i) {
		if (snprintf(node, sizeof(node), str, DRM_DIR_NAME,
			     DRM_RENDER_NODE_START + i) < 0)
			continue;

		fd = open(node, O_RDWR, 0);
		if (fd < 0)
			continue;

		if (is_virtio_gpu_owned_by_lic(fd)) {
			ALOGI("Skip virtio-GPU owned by LIC: %s\n", node);
			close(fd);
			continue;
		}

		drmVersionPtr version = drmGetVersion(fd);
		if (!version) {
			close(fd);
			continue;
		}

		drmFreeVersion(version);
		if (get_gpu_type(fd) == GPU_GRP_TYPE_VIRTIO_GPU_IVSHMEM_IDX) {
			drv_logi("New added node is virtio-ivishmem node");
			gpu_grp_type_ |= GPU_GRP_TYPE_HAS_VIRTIO_GPU_IVSHMEM_BIT;
			struct driver *drv = drv_create(fd, gpu_grp_type_);
			if (!drv) {
				drv_loge("Failed to create driver\n");
				close(fd);
				continue;
			}
			drivers_[GPU_GRP_TYPE_VIRTIO_GPU_IVSHMEM_IDX] = drv;
			drv_ivshmem_ = drv;
			return 0;
		} else {
			drv_logi("New added node is NOT virtio-ivishmem node");
			close(fd);
			continue;
		}
	}
	return -ENODEV;
}

cros_gralloc_driver::cros_gralloc_driver(): drivers_(GPU_GRP_TYPE_NR, nullptr)
{
	/*
	 * Create a driver from render nodes first, then try card
	 * nodes.
	 *
	 * TODO(gsingh): Enable render nodes on udl/evdi.
	 */

	char const *render_nodes_fmt = "%s/renderD%d";
	uint32_t num_nodes = DRM_NUM_NODES;
	uint32_t min_render_node = DRM_RENDER_NODE_START;
	uint32_t max_render_node = (min_render_node + num_nodes);

	const char *undesired[2] = { "vgem", nullptr };
	uint32_t j;
	char node[64];
	int fd;
	int fallback_fd = -1;
	const int render_num = 10;
	std::vector<int> driver_fds{GPU_GRP_TYPE_NR, -1};

	char buf[PROP_VALUE_MAX];
	property_get("ro.product.device", buf, "unknown");
	mt8183_camera_quirk_ = !strncmp(buf, "kukui", strlen("kukui"));

	for (uint32_t i = min_render_node; i < max_render_node; i++) {
		if (snprintf(node, sizeof(node), render_nodes_fmt,
			     DRM_DIR_NAME, i) < 0)
			continue;

		fd = open(node, O_RDWR, 0);
		if (fd < 0)
			continue;

		if (is_virtio_gpu_owned_by_lic(fd)) {
			ALOGI("Skip virtio-GPU owned by LIC: %s\n", node);
			close(fd);
			continue;
		}

		drmVersionPtr version = drmGetVersion(fd);
		if (!version) {
			close(fd);
			continue;
		}

		for (j = 0; j < ARRAY_SIZE(undesired); j++) {
			if (undesired[j] && !strcmp(version->name, undesired[j])) {
				drmFreeVersion(version);
				close(fd);
				break;
			}
		}

		// hit any of undesired render node
		if (j < ARRAY_SIZE(undesired))
			continue;

		if (fallback_fd == -1)
			fallback_fd = fd;

		// We have several kinds of virtio-GPU devices:
		//
		// * virtio-GPU supporting blob feature: normal case implemented by ACRN device
		//   model in SOS. This kind of device is able to import GEM objects from other
		//   deivces such as Intel GPUs. Hence, for the sake of performance, we would like
		//   to allocate scan-out buffers from Intel GPUs because in this way 1) the buffers
		//   are allowed to reside in local memory if the rendering GPU is a descrete one,
		//   2) it's easier to support tiled buffers. Depending on whether allow-p2p feature
		//   is enabled or not, the devices of this kind can be divided into two subclasses:
		//
		//   * If allow-p2p is not supported, the (physical) display is backed by iGPU;
		//   * Otherwise, the display is backed by dGPU.
		//
		//   The backing display matters because 1) dGPU scans out buffers if and only if
		//   the buffers reside in local memory, whereas iGPU scans out system memory
		//   buffers only, 2) iGPU and dGPU support different set of tiling formats, which
		//   is a headache if we render with dGPU and display with iGPU and vice versa.
		//
		// * virtio-GPU not supporting blob feature: QNX hypervisor case and Redhat's use
		//   case. Being incapable of importing external buffers, scan-out buffers are
		//   required to be allocated by the virtio-GPU itself.
		//
		// * virtio-GPU backed by inter-VM shared-memory (ivshmem): inter-VM screen cast use
		//   case. This kind doesn't support importing external buffers neither, and it's
		//   needed only when the buffers shall be shared for casting.
		int gpu_grp_type_idx = get_gpu_type(fd);

		if (gpu_grp_type_idx != -1 &&
		    !(gpu_grp_type_ & (1ull << gpu_grp_type_idx))) {
			gpu_grp_type_ |= (1ull << gpu_grp_type_idx);
			driver_fds[gpu_grp_type_idx] = fd;
		} else if (fd != fallback_fd) {
			close(fd);
		}
		drmFreeVersion(version);
	}

restart:
	for (int i = 0; i < GPU_GRP_TYPE_NR; ++i) {
		if (!(gpu_grp_type_ & (1ull << i)))
			continue;
		if (drivers_[i] != nullptr) {
			DRV_DESTROY(drivers_[i]);
		}
		struct driver *drv = drv_create(driver_fds[i], gpu_grp_type_);
		if (!drv) {
			drv_loge("failed to init minigbm driver on device of type %d\n", i);
			close(driver_fds[i]);
			driver_fds[i] = -1;
			gpu_grp_type_ &= ~(1ull << i);
			goto restart;
		}
		drivers_[i] = drv;

		/* We don't use fallback driver at all when we have known device. */
		if (driver_fds[i] == fallback_fd)
			fallback_fd = -1;
	}

	if (gpu_grp_type_ == 0) {
		drv_loge("No known device found!\n");
		if (fallback_fd >= 0) {
			drv_fallback_ = drv_create(fallback_fd, gpu_grp_type_);
			drv_render_ = drv_kms_ = drv_video_ = drv_fallback_;
		}
		return;
	}

	if (fallback_fd >= 0) {
		close(fallback_fd);
		fallback_fd = -1;
	}

	int idx = select_render_driver(gpu_grp_type_);
	if (idx != -1) {
		drv_render_ = drivers_[idx];
	}
	idx = select_kms_driver(gpu_grp_type_);
	if (idx != -1) {
		drv_kms_ = drivers_[idx];
	}
	idx = select_video_driver(gpu_grp_type_);
	if (idx != -1) {
		drv_video_ = drivers_[idx];
	}
	if (gpu_grp_type_ & GPU_GRP_TYPE_HAS_VIRTIO_GPU_IVSHMEM_BIT) {
		drv_ivshmem_ = drivers_[GPU_GRP_TYPE_VIRTIO_GPU_IVSHMEM_IDX];
	}
}

cros_gralloc_driver::~cros_gralloc_driver()
{
	buffers_.clear();
	handles_.clear();
	if (gpu_grp_type_ == 0) {
		DRV_DESTROY(drv_fallback_);
		return;
	}

	for (int i = 0; i < GPU_GRP_TYPE_NR; ++i) {
		if (gpu_grp_type_ & (1ull << i)) {
			DRV_DESTROY(drivers_[i]);
		}
	}
}

bool cros_gralloc_driver::is_initialized()
{
	return (drv_render_ != nullptr);
}

bool cros_gralloc_driver::is_video_format(const struct cros_gralloc_buffer_descriptor *descriptor)
{
	if (!IsSupportedYUVFormat(descriptor->droid_format))
		return false;

	// if supported yuv format, almost is_video==TRUE, except check flex implementation.
	// In this case only camera usage is video format, according to 'drv_resolve_format_and_use_flags_helper()'.
	if (descriptor->drm_format == DRM_FORMAT_FLEX_IMPLEMENTATION_DEFINED)
		return (descriptor->use_flags & (BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE));

	return true;
}

bool cros_gralloc_driver::use_ivshm_drv(const struct cros_gralloc_buffer_descriptor *descriptor, bool retain)
{
	// To keep original code logic, when calling retain API, we don't have SCANOUT flag
	if (retain) {
		return (descriptor->width == IVSH_WIDTH) &&
		       (descriptor->height == IVSH_HEIGHT);
	}

	return (descriptor->width == IVSH_WIDTH) &&
	       (descriptor->height == IVSH_HEIGHT) &&
	       (descriptor->use_flags & BO_USE_SCANOUT);
}

struct driver *cros_gralloc_driver::select_driver(const struct cros_gralloc_buffer_descriptor *descriptor, bool retain)
{
	if (use_ivshm_drv(descriptor, retain)) {
		// Give it a chance to re-scan devices.
		if (drv_ivshmem_ == nullptr) {
			reload();
		}
		if (drv_ivshmem_) {
			return drv_ivshmem_;
		}
	}
	if (is_video_format(descriptor)) {
		return drv_video_;
	}
	if (descriptor->use_flags & BO_USE_SCANOUT) {
		return drv_kms_;
	}
	return drv_render_;
}

bool cros_gralloc_driver::get_resolved_format_and_use_flags(
    const struct cros_gralloc_buffer_descriptor *descriptor, uint32_t *out_format,
    uint64_t *out_use_flags)
{
	uint32_t resolved_format;
	uint64_t resolved_use_flags;
	struct combination *combo;

	struct driver *drv = select_driver(descriptor);

	if (mt8183_camera_quirk_ && (descriptor->use_flags & BO_USE_CAMERA_READ) &&
	    !(descriptor->use_flags & BO_USE_SCANOUT) &&
	    descriptor->drm_format == DRM_FORMAT_FLEX_IMPLEMENTATION_DEFINED) {
		*out_use_flags = descriptor->use_flags;
		*out_format = DRM_FORMAT_MTISP_SXYZW10;
		return true;
	}

	drv_resolve_format_and_use_flags(drv, descriptor->drm_format, descriptor->use_flags,
					 &resolved_format, &resolved_use_flags);

	combo = drv_get_combination(drv, resolved_format, resolved_use_flags);
	if (!combo && (descriptor->use_flags & BO_USE_SCANOUT)) {
		resolved_use_flags &= ~BO_USE_SCANOUT;
		combo = drv_get_combination(drv, resolved_format, descriptor->use_flags);
	}
	if (!combo && (descriptor->droid_usage & GRALLOC_USAGE_HW_VIDEO_ENCODER) &&
	    descriptor->droid_format != HAL_PIXEL_FORMAT_YCbCr_420_888) {
		// Unmask BO_USE_HW_VIDEO_ENCODER for other formats. They are mostly
		// intermediate formats not passed directly to the encoder (e.g.
		// camera). YV12 is passed to the encoder component, but it is converted
		// to YCbCr_420_888 before being passed to the hw encoder.
		resolved_use_flags &= ~BO_USE_HW_VIDEO_ENCODER;
		combo = drv_get_combination(drv, resolved_format, resolved_use_flags);
	}
	if (!combo && (descriptor->droid_usage & BUFFER_USAGE_FRONT_RENDERING_MASK)) {
		resolved_use_flags &= ~BO_USE_FRONT_RENDERING;
		resolved_use_flags |= BO_USE_LINEAR;
		combo = drv_get_combination(drv, resolved_format, resolved_use_flags);
	}
	if (!combo)
		return false;

	*out_format = resolved_format;
	*out_use_flags = resolved_use_flags;
	return true;
}

bool cros_gralloc_driver::is_supported(const struct cros_gralloc_buffer_descriptor *descriptor)
{
	uint32_t resolved_format;
	uint64_t resolved_use_flags;
	struct driver *drv = select_driver(descriptor);
	uint32_t max_texture_size = drv_get_max_texture_2d_size(drv);
	if (!get_resolved_format_and_use_flags(descriptor, &resolved_format, &resolved_use_flags))
		return false;
	// Allow blob buffers to go beyond the limit.
	if (descriptor->droid_format == HAL_PIXEL_FORMAT_BLOB)
		return true;

	return descriptor->width <= max_texture_size && descriptor->height <= max_texture_size;
}

int cros_gralloc_driver::create_reserved_region(const std::string &buffer_name,
						uint64_t reserved_region_size)
{
	int ret;

#if ANDROID_API_LEVEL >= 31 && defined(HAS_DMABUF_SYSTEM_HEAP)
	ret = allocator_.Alloc(kDmabufSystemHeapName, reserved_region_size);
	if (ret >= 0)
		return ret;
#endif

	ret = memfd_create_reserved_region(buffer_name, reserved_region_size);
	if (ret >= 0)
		return ret;

	ALOGE("Failed to create_reserved_region.");
	return -1;
}

int32_t cros_gralloc_driver::allocate(const struct cros_gralloc_buffer_descriptor *descriptor,
				      native_handle_t **out_handle)
{
#ifdef USE_GRALLOC1
	uint64_t mod;
#endif
	int ret = 0;
	size_t num_planes;
	size_t num_fds;
	size_t num_ints;
	uint32_t resolved_format;
	uint32_t bytes_per_pixel;
	uint64_t resolved_use_flags;
	struct bo *bo;
	struct cros_gralloc_handle *hnd;
	std::unique_ptr<cros_gralloc_buffer> buffer;

	struct driver *drv = select_driver(descriptor);

	if (!get_resolved_format_and_use_flags(descriptor, &resolved_format, &resolved_use_flags)) {
		ALOGE("Failed to resolve format and use_flags.");
		return -EINVAL;
	}

        /*
         * TODO(b/79682290): ARC++ assumes NV12 is always linear and doesn't
         * send modifiers across Wayland protocol, so we or in the
         * BO_USE_LINEAR flag here. We need to fix ARC++ to allocate and work
         * with tiled buffers.
         */
        if (resolved_format == DRM_FORMAT_NV12)
                resolved_use_flags |= BO_USE_LINEAR;

        /*
         * This unmask is a backup in the case DRM_FORMAT_FLEX_IMPLEMENTATION_DEFINED is resolved
         * to non-YUV formats.
         */
        if (descriptor->drm_format == DRM_FORMAT_FLEX_IMPLEMENTATION_DEFINED &&
            (resolved_format == DRM_FORMAT_XBGR8888 || resolved_format == DRM_FORMAT_ABGR8888)) {
                resolved_use_flags &= ~BO_USE_HW_VIDEO_ENCODER;
        }

	if (descriptor->modifier == 0) {
		bo = drv_bo_create(drv, descriptor->width, descriptor->height,
				   resolved_format, resolved_use_flags);
	} else {
		bo = drv_bo_create_with_modifiers(drv, descriptor->width, descriptor->height,
						  resolved_format, &descriptor->modifier, 1);
	}

	if (!bo) {
		ALOGE("Failed to create bo.");
		return -errno;
	}

	/*
	 * If there is a desire for more than one kernel buffer, this can be
	 * removed once the ArcCodec and Wayland service have the ability to
	 * send more than one fd. GL/Vulkan drivers may also have to modified.
	 */
	if (drv_num_buffers_per_bo(bo) != 1) {
		ALOGE("Can only support one buffer per bo.");
		goto destroy_bo;
	}

	num_planes = drv_bo_get_num_planes(bo);
	num_fds = num_planes;

	if (descriptor->reserved_region_size > 0)
		num_fds += 1;

	num_ints = ((sizeof(struct cros_gralloc_handle) - sizeof(native_handle_t)) / sizeof(int)) -
		   num_fds;

	hnd =
	    reinterpret_cast<struct cros_gralloc_handle *>(native_handle_create(num_fds, num_ints));

	for (size_t i = 0; i < DRV_MAX_FDS; i++)
		hnd->fds[i] = -1;

	hnd->num_planes = num_planes;
	for (size_t plane = 0; plane < num_planes; plane++) {
		ret = drv_bo_get_plane_fd(bo, plane);
		if (ret < 0)
			goto destroy_hnd;

		hnd->fds[plane] = ret;
		hnd->strides[plane] = drv_bo_get_plane_stride(bo, plane);
		hnd->offsets[plane] = drv_bo_get_plane_offset(bo, plane);
		hnd->sizes[plane] = drv_bo_get_plane_size(bo, plane);
#ifdef USE_GRALLOC1
		mod = drv_bo_get_format_modifier(bo);
		hnd->format_modifiers[2 * plane] = static_cast<uint32_t>(mod >> 32);
		hnd->format_modifiers[2 * plane + 1] = static_cast<uint32_t>(mod);
#endif
	}

	hnd->reserved_region_size = descriptor->reserved_region_size;
	if (hnd->reserved_region_size > 0) {
		ret = create_reserved_region(descriptor->name, hnd->reserved_region_size);
		if (ret < 0)
			goto destroy_hnd;

		hnd->fds[hnd->num_planes] = ret;
	}

	static std::atomic<uint32_t> next_buffer_id{ 1 };
	hnd->id = next_buffer_id++;
	hnd->width = drv_bo_get_width(bo);
	hnd->height = drv_bo_get_height(bo);
	hnd->format = drv_bo_get_format(bo);
	hnd->tiling = drv_bo_get_tiling(bo);
	hnd->format_modifier = drv_bo_get_format_modifier(bo);
	hnd->use_flags = drv_bo_get_use_flags(bo);
	bytes_per_pixel = drv_bytes_per_pixel_from_format(hnd->format, 0);
	hnd->pixel_stride = DIV_ROUND_UP(hnd->strides[0], bytes_per_pixel);
	hnd->magic = cros_gralloc_magic;
#ifdef USE_GRALLOC1
	hnd->producer_usage = descriptor->producer_usage;
	hnd->consumer_usage = descriptor->consumer_usage;
#endif
	hnd->droid_format = descriptor->droid_format;
	hnd->usage = descriptor->droid_usage;
	hnd->total_size = descriptor->reserved_region_size + drv_bo_get_total_size(bo);

	buffer = cros_gralloc_buffer::create(bo, hnd);
	if (!buffer) {
		ALOGE("Failed to allocate: failed to create cros_gralloc_buffer.");
		ret = -1;
		goto destroy_hnd;
	}

	{
		std::lock_guard<std::mutex> lock(mutex_);

		struct cros_gralloc_imported_handle_info hnd_info = {
			.buffer = buffer.get(),
			.refcount = 1,
		};
		handles_.emplace(hnd, hnd_info);
		buffers_.emplace(hnd->id, std::move(buffer));
	}

	*out_handle = hnd;
	return 0;

destroy_hnd:
	native_handle_close(hnd);
	native_handle_delete(hnd);

destroy_bo:
	drv_bo_destroy(bo);
	return ret;
}

int32_t cros_gralloc_driver::retain(buffer_handle_t handle)
{
	std::lock_guard<std::mutex> lock(mutex_);
	struct driver *drv;

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		ALOGE("Invalid handle.");
		return -EINVAL;
	}

	struct cros_gralloc_buffer_descriptor descriptor = {
		.droid_format = hnd->droid_format,
		.drm_format = hnd->format,
		.use_flags = hnd->use_flags,
	};
	drv = select_driver(&descriptor, true);

	auto hnd_it = handles_.find(hnd);
	if (hnd_it != handles_.end()) {
		// The underlying buffer (as multiple handles can refer to the same buffer)
		// has already been imported into this process and the given handle has
		// already been registered in this process. Increase both the buffer and
		// handle reference count.
		auto &hnd_info = hnd_it->second;

		hnd_info.buffer->increase_refcount();
		hnd_info.refcount++;

		return 0;
	}

	uint32_t id = hnd->id;

	cros_gralloc_buffer *buffer = nullptr;

	auto buffer_it = buffers_.find(id);
	if (buffer_it != buffers_.end()) {
		// The underlying buffer (as multiple handles can refer to the same buffer)
		// has already been imported into this process but the given handle has not
		// yet been registered. Increase the buffer reference count (here) and start
		// to track the handle (below).
		buffer = buffer_it->second.get();
		buffer->increase_refcount();
	} else {
		// The underlying buffer has not yet been imported into this process. Import
		// and start to track the buffer (here) and start to track the handle (below).
		struct drv_import_fd_data data = {
			.format_modifier = hnd->format_modifier,
			.width = hnd->width,
			.height = hnd->height,
			.format = hnd->format,
			.tiling = hnd->tiling,
			.use_flags = hnd->use_flags,
		};
		memcpy(data.fds, hnd->fds, sizeof(data.fds));
		memcpy(data.strides, hnd->strides, sizeof(data.strides));
		memcpy(data.offsets, hnd->offsets, sizeof(data.offsets));

		struct bo *bo = drv_bo_import(drv, &data);
		if (!bo)
			return -EFAULT;

		auto scoped_buffer = cros_gralloc_buffer::create(bo, hnd);
		if (!scoped_buffer) {
			ALOGE("Failed to import: failed to create cros_gralloc_buffer.");
			return -1;
		}
		buffer = scoped_buffer.get();
		buffers_.emplace(id, std::move(scoped_buffer));
	}

	struct cros_gralloc_imported_handle_info hnd_info = {
		.buffer = buffer,
		.refcount = 1,
	};
	handles_.emplace(hnd, hnd_info);
	return 0;
}

int32_t cros_gralloc_driver::release(buffer_handle_t handle)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		ALOGE("Invalid handle.");
		return -EINVAL;
	}

	auto buffer = get_buffer(hnd);
	if (!buffer) {
		ALOGE("Invalid reference (release() called on unregistered handle).");
		return -EINVAL;
	}

	if (!--handles_[hnd].refcount)
		handles_.erase(hnd);

	if (buffer->decrease_refcount() == 0) {
		buffers_.erase(buffer->get_id());
	}

	return 0;
}

int32_t cros_gralloc_driver::lock(buffer_handle_t handle, int32_t acquire_fence,
				  bool close_acquire_fence, const struct rectangle *rect,
				  uint32_t map_flags, uint8_t *addr[DRV_MAX_PLANES])
{
	int32_t ret = cros_gralloc_sync_wait(acquire_fence, close_acquire_fence);
	if (ret)
		return ret;

	std::lock_guard<std::mutex> lock(mutex_);

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		ALOGE("Invalid handle.");
		return -EINVAL;
	}

	auto buffer = get_buffer(hnd);
	if (!buffer) {
		ALOGE("Invalid reference (lock() called on unregistered handle).");
		return -EINVAL;
	}

	return buffer->lock(rect, map_flags, addr);
}

#ifdef USE_GRALLOC1
int32_t cros_gralloc_driver::lock(buffer_handle_t handle, int32_t acquire_fence, uint32_t map_flags,
				  uint8_t *addr[DRV_MAX_PLANES])
{
	int32_t ret = cros_gralloc_sync_wait(acquire_fence);
	if (ret)
		return ret;

	std::lock_guard<std::mutex> lock(mutex_);
	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		ALOGE("Invalid handle.");
		return -EINVAL;
	}

	auto buffer = get_buffer(hnd);
	if (!buffer) {
		ALOGE("Invalid Reference.");
		return -EINVAL;
	}

	return buffer->lock(map_flags, addr);
}
#endif

int32_t cros_gralloc_driver::unlock(buffer_handle_t handle, int32_t *release_fence)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		ALOGE("Invalid handle.");
		return -EINVAL;
	}

	auto buffer = get_buffer(hnd);
	if (!buffer) {
		ALOGE("Invalid reference (unlock() called on unregistered handle).");
		return -EINVAL;
	}

	/*
	 * From the ANativeWindow::dequeueBuffer documentation:
	 *
	 * "A value of -1 indicates that the caller may access the buffer immediately without
	 * waiting on a fence."
	 */
	*release_fence = -1;
	return buffer->unlock();
}

int32_t cros_gralloc_driver::invalidate(buffer_handle_t handle)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		ALOGE("Invalid handle.");
		return -EINVAL;
	}

	auto buffer = get_buffer(hnd);
	if (!buffer) {
		ALOGE("Invalid reference (invalidate() called on unregistered handle).");
		return -EINVAL;
	}

	return buffer->invalidate();
}

int32_t cros_gralloc_driver::flush(buffer_handle_t handle)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		ALOGE("Invalid handle.");
		return -EINVAL;
	}

	auto buffer = get_buffer(hnd);
	if (!buffer) {
		ALOGE("Invalid reference (flush() called on unregistered handle).");
		return -EINVAL;
	}

	return buffer->flush();
}

int32_t cros_gralloc_driver::get_backing_store(buffer_handle_t handle, uint64_t *out_store)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		ALOGE("Invalid handle.");
		return -EINVAL;
	}

	*out_store = static_cast<uint64_t>(hnd->id);
	return 0;
}

int32_t cros_gralloc_driver::resource_info(buffer_handle_t handle, uint32_t strides[DRV_MAX_PLANES],
					   uint32_t offsets[DRV_MAX_PLANES],
					   uint64_t *format_modifier)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		ALOGE("Invalid handle.");
		return -EINVAL;
	}

	auto buffer = get_buffer(hnd);
	if (!buffer) {
		ALOGE("Invalid reference (resource_info() called on unregistered handle).");
		return -EINVAL;
	}

	return buffer->resource_info(strides, offsets, format_modifier);
}

int32_t cros_gralloc_driver::get_reserved_region(buffer_handle_t handle,
						 void **reserved_region_addr,
						 uint64_t *reserved_region_size)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		ALOGE("Invalid handle.");
		return -EINVAL;
	}

	auto buffer = get_buffer(hnd);
	if (!buffer) {
		ALOGE("Invalid reference (get_reserved_region() called on unregistered handle).");
		return -EINVAL;
	}

	return buffer->get_reserved_region(reserved_region_addr, reserved_region_size);
}

uint32_t cros_gralloc_driver::get_resolved_drm_format(uint32_t drm_format, uint64_t use_flags)
{
	uint32_t resolved_format;
	uint64_t resolved_use_flags;
	struct driver *drv = drv_render_;

	drv_resolve_format_and_use_flags(drv, drm_format, use_flags, &resolved_format,
					 &resolved_use_flags);

	return resolved_format;
}

uint32_t cros_gralloc_driver::get_resolved_common_drm_format(uint32_t drm_format)
{
	return drv_resolved_common_drm_format(drm_format);
}


cros_gralloc_buffer *cros_gralloc_driver::get_buffer(cros_gralloc_handle_t hnd)
{
	/* Assumes driver mutex is held. */
	if (handles_.count(hnd))
		return handles_[hnd].buffer;

	return nullptr;
}

void cros_gralloc_driver::with_buffer(cros_gralloc_handle_t hnd,
				      const std::function<void(cros_gralloc_buffer *)> &function)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto buffer = get_buffer(hnd);
	if (!buffer) {
		ALOGE("Invalid reference (with_buffer() called on unregistered handle).");
		return;
	}

	function(buffer);
}

void cros_gralloc_driver::with_each_buffer(
    const std::function<void(cros_gralloc_buffer *)> &function)
{
	std::lock_guard<std::mutex> lock(mutex_);

	for (const auto &pair : buffers_)
		function(pair.second.get());
}

int cros_gralloc_driver::select_render_driver(uint64_t gpu_grp_type)
{
	if (gpu_grp_type & GPU_GRP_TYPE_HAS_INTEL_DGPU_BIT) {
		return GPU_GRP_TYPE_INTEL_DGPU_IDX;
	}
	if (gpu_grp_type & GPU_GRP_TYPE_HAS_INTEL_IGPU_BIT) {
		return GPU_GRP_TYPE_INTEL_IGPU_IDX;
	}
	if (gpu_grp_type & GPU_GRP_TYPE_HAS_VIRTIO_GPU_BLOB_BIT) {
		return GPU_GRP_TYPE_VIRTIO_GPU_BLOB_IDX;
	}
	if (gpu_grp_type & GPU_GRP_TYPE_HAS_VIRTIO_GPU_BLOB_P2P_BIT) {
		return GPU_GRP_TYPE_VIRTIO_GPU_BLOB_P2P_IDX;
	}
	if (gpu_grp_type & GPU_GRP_TYPE_HAS_VIRTIO_GPU_NO_BLOB_BIT) {
		return GPU_GRP_TYPE_VIRTIO_GPU_NO_BLOB_IDX;
	}
	if (gpu_grp_type & GPU_GRP_TYPE_HAS_VIRTIO_GPU_IVSHMEM_BIT) {
		return GPU_GRP_TYPE_VIRTIO_GPU_IVSHMEM_IDX;
	}
	return -1;
}

int cros_gralloc_driver::select_kms_driver(uint64_t gpu_grp_type)
{
	// virtio-GPU without blob cannot import external buffers.
	if (gpu_grp_type & GPU_GRP_TYPE_HAS_VIRTIO_GPU_NO_BLOB_BIT) {
		return GPU_GRP_TYPE_VIRTIO_GPU_NO_BLOB_IDX;
	}
	// QNX case: virtio-GPU is ivshmem backed device and cannot import external buffers.
	// Screen cast also uses ivshmem virtio-GPU but we have virtio-GPU supporting blob.
	if ((gpu_grp_type & GPU_GRP_TYPE_HAS_VIRTIO_GPU_IVSHMEM_BIT) &&
	    !(gpu_grp_type & GPU_GRP_TYPE_HAS_VIRTIO_GPU_BLOB_BIT) &&
	    !(gpu_grp_type & GPU_GRP_TYPE_HAS_VIRTIO_GPU_BLOB_P2P_BIT)) {
		return GPU_GRP_TYPE_VIRTIO_GPU_IVSHMEM_IDX;
	}
	if (gpu_grp_type & GPU_GRP_TYPE_HAS_INTEL_DGPU_BIT) {
		return GPU_GRP_TYPE_INTEL_DGPU_IDX;
	}
	if (gpu_grp_type & GPU_GRP_TYPE_HAS_INTEL_IGPU_BIT) {
		return GPU_GRP_TYPE_INTEL_IGPU_IDX;
	}
	if (gpu_grp_type & GPU_GRP_TYPE_HAS_VIRTIO_GPU_BLOB_BIT) {
		return GPU_GRP_TYPE_VIRTIO_GPU_BLOB_IDX;
	}
	if (gpu_grp_type & GPU_GRP_TYPE_HAS_VIRTIO_GPU_BLOB_P2P_BIT) {
		return GPU_GRP_TYPE_VIRTIO_GPU_BLOB_P2P_IDX;
	}
	if (gpu_grp_type & GPU_GRP_TYPE_HAS_VIRTIO_GPU_IVSHMEM_BIT) {
		return GPU_GRP_TYPE_VIRTIO_GPU_IVSHMEM_IDX;
	}
	return -1;
}

int cros_gralloc_driver::select_video_driver(uint64_t gpu_grp_type)
{
	if (gpu_grp_type & GPU_GRP_TYPE_HAS_INTEL_DGPU_BIT) {
		return GPU_GRP_TYPE_INTEL_DGPU_IDX;
	}
	if (gpu_grp_type & GPU_GRP_TYPE_HAS_INTEL_IGPU_BIT) {
		return GPU_GRP_TYPE_INTEL_IGPU_IDX;
	}
	if (gpu_grp_type & GPU_GRP_TYPE_HAS_VIRTIO_GPU_BLOB_BIT) {
		return GPU_GRP_TYPE_VIRTIO_GPU_BLOB_IDX;
	}
	if (gpu_grp_type & GPU_GRP_TYPE_HAS_VIRTIO_GPU_BLOB_P2P_BIT) {
		return GPU_GRP_TYPE_VIRTIO_GPU_BLOB_P2P_IDX;
	}
	if (gpu_grp_type & GPU_GRP_TYPE_HAS_VIRTIO_GPU_NO_BLOB_BIT) {
		return GPU_GRP_TYPE_VIRTIO_GPU_NO_BLOB_IDX;
	}
	if (gpu_grp_type & GPU_GRP_TYPE_HAS_VIRTIO_GPU_IVSHMEM_BIT) {
		return GPU_GRP_TYPE_VIRTIO_GPU_IVSHMEM_IDX;
	}
	return -1;
}

