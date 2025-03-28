/*
 * Copyright 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "drv.h"
#ifdef DRV_I915

#include <assert.h>
#include <cpuid.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <cutils/properties.h>

#include "drv_helpers.h"
#include "drv_priv.h"
// #include "external/i915_drm.h"
#include "util.h"
#include "i915_prelim.h"
#include "intel_device.h"

#define I915_CACHELINE_SIZE 64
#define I915_CACHELINE_MASK (I915_CACHELINE_SIZE - 1)

#define GEN_VERSION_X10(dev)	((dev)->graphics_version * 10 + (dev)->sub_version)

static bool is_prelim_kernel = false;
static const uint32_t scanout_render_formats[] = { DRM_FORMAT_ABGR2101010, DRM_FORMAT_ABGR8888,
						   DRM_FORMAT_ARGB2101010, DRM_FORMAT_ARGB8888,
						   DRM_FORMAT_RGB565,	   DRM_FORMAT_XBGR2101010,
						   DRM_FORMAT_XBGR8888,	   DRM_FORMAT_XRGB2101010,
						   DRM_FORMAT_XRGB8888 };

static const uint32_t render_formats[] = { DRM_FORMAT_ABGR16161616F };

static const uint32_t texture_only_formats[] = { DRM_FORMAT_R8, DRM_FORMAT_NV12, DRM_FORMAT_P010,
						 DRM_FORMAT_YVU420, DRM_FORMAT_YVU420_ANDROID,
						 DRM_FORMAT_YUYV };

static const uint32_t linear_source_formats[] = { DRM_FORMAT_R16,    DRM_FORMAT_NV16,
                                                 DRM_FORMAT_YUV420, DRM_FORMAT_YUV422,
                                                 DRM_FORMAT_YUV444, DRM_FORMAT_NV21,
                                                 DRM_FORMAT_P010 };

static const uint32_t source_formats[] = { DRM_FORMAT_P010_INTEL, DRM_FORMAT_NV12_Y_TILED_INTEL };

struct iris_memregion {
	struct drm_i915_gem_memory_class_instance region;
	uint64_t size;
};

static const char *tiling_to_string(int tiling) {
	switch (tiling) {
	case I915_TILING_NONE:
		return "linear";
	case I915_TILING_X:
		return "tiling-x";
	case I915_TILING_Y:
		return "tiling-y";
	case I915_TILING_4:
		return "tiling-4";
	}
	return "unknown";
}

#if !defined(DRM_CAP_CURSOR_WIDTH)
#define DRM_CAP_CURSOR_WIDTH 0x8
#endif

#if !defined(DRM_CAP_CURSOR_HEIGHT)
#define DRM_CAP_CURSOR_HEIGHT 0x9
#endif

static const uint32_t kDefaultCursorWidth = 64;
static const uint32_t kDefaultCursorHeight = 64;

#define BO_USE_CAMERA_MASK BO_USE_CAMERA_READ | BO_USE_SCANOUT | BO_USE_CAMERA_WRITE


static const uint64_t gen_modifier_order[] = { I915_FORMAT_MOD_Y_TILED_CCS, I915_FORMAT_MOD_Y_TILED,
					       I915_FORMAT_MOD_X_TILED, DRM_FORMAT_MOD_LINEAR };

static const uint64_t gen12_modifier_order[] = { I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS,
						 I915_FORMAT_MOD_Y_TILED, I915_FORMAT_MOD_X_TILED,
						 DRM_FORMAT_MOD_LINEAR };

static const uint64_t gen11_modifier_order[] = { I915_FORMAT_MOD_Y_TILED, I915_FORMAT_MOD_X_TILED,
						 DRM_FORMAT_MOD_LINEAR };

static const uint64_t xe_lpdp_modifier_order[] = { I915_FORMAT_MOD_4_TILED, I915_FORMAT_MOD_X_TILED,
						   DRM_FORMAT_MOD_LINEAR };

struct modifier_support_t {
	const uint64_t *order;
	uint32_t count;
};

struct i915_device {
	uint32_t graphics_version;
	uint32_t sub_version;
	struct modifier_support_t modifier;
	int device_id;
	struct iris_memregion vram, sys;
	uint64_t cursor_width;
	uint64_t cursor_height;

	uint32_t has_llc 			: 1;
	uint32_t has_hw_protection	: 1;
	uint32_t is_xelpd			: 1;
	uint32_t has_mmap_offset	: 1;
	uint32_t has_local_mem		: 1;
	uint32_t force_mem_local	: 1;
};

/*
 * src/gallium/drivers/iris/iris_bufmgr.c
 */

enum iris_heap {
	IRIS_HEAP_SYSTEM_MEMORY,
	IRIS_HEAP_DEVICE_LOCAL,
	IRIS_HEAP_DEVICE_LOCAL_PREFERRED,
	IRIS_HEAP_MAX,
};

const char *
iris_heap_to_string[IRIS_HEAP_MAX] = {
	[IRIS_HEAP_SYSTEM_MEMORY] = "system",
	[IRIS_HEAP_DEVICE_LOCAL] = "local",
	[IRIS_HEAP_DEVICE_LOCAL_PREFERRED] = "local-preferred",
};

static enum iris_heap
flags_to_heap(struct i915_device *i915, unsigned flags)
{
	if (i915->vram.size > 0) {
		return IRIS_HEAP_DEVICE_LOCAL_PREFERRED;
	} else {
		return IRIS_HEAP_SYSTEM_MEMORY;
	}
}

bool i915_has_tile4(struct i915_device *i915)
{
	return GEN_VERSION_X10(i915) >= 125;
}

static void i915_get_modifier_order(struct i915_device *i915)
{
	if (i915_has_tile4(i915)) {
		i915->modifier.order = xe_lpdp_modifier_order;
		i915->modifier.count = ARRAY_SIZE(xe_lpdp_modifier_order);
	} else if (i915->graphics_version == 12) {
		i915->modifier.order = gen12_modifier_order;
		i915->modifier.count = ARRAY_SIZE(gen12_modifier_order);
	} else if (i915->graphics_version == 11) {
		i915->modifier.order = gen11_modifier_order;
		i915->modifier.count = ARRAY_SIZE(gen11_modifier_order);
	} else {
		i915->modifier.order = gen_modifier_order;
		i915->modifier.count = ARRAY_SIZE(gen_modifier_order);
	}
}

static uint64_t unset_flags(uint64_t current_flags, uint64_t mask)
{
	uint64_t value = current_flags & ~mask;
	return value;
}

/*
 * Check virtual machine type, by checking cpuid
 */
enum {
	HYPERTYPE_NONE 	    = 0,
	HYPERTYPE_ANY       = 0x1,
	HYPERTYPE_TYPE_ACRN = 0x2,
	HYPERTYPE_TYPE_KVM  = 0x4
};
static inline int vm_type()
{
	int type = HYPERTYPE_NONE;
	union {
		uint32_t sig32[3];
		char text[13];
	} sig = {};

	uint32_t eax=0, ebx=0, ecx=0, edx=0;
	if(__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
		if (((ecx >> 31) & 1) == 1) {
			type |= HYPERTYPE_ANY;

			__cpuid(0x40000000U, eax, ebx, ecx, edx);
			sig.sig32[0] = ebx;
			sig.sig32[1] = ecx;
			sig.sig32[2] = edx;
			if (!strncmp(sig.text, "ACRNACRNACRN", 12))
				type |= HYPERTYPE_TYPE_ACRN;
			else if ((!strncmp(sig.text, "KVMKVMKVM", 9)) ||
				 (!strncmp(sig.text, "EVMMEVMMEVMM", 12)))
				type |= HYPERTYPE_TYPE_KVM;
		}
	}
	return type;
}

static int i915_add_combinations(struct driver *drv)
{
	struct i915_device *i915 = drv->priv;

	const uint64_t scanout_and_render = BO_USE_RENDER_MASK | BO_USE_SCANOUT;
	const uint64_t render = BO_USE_RENDER_MASK;
	const uint64_t texture_only = BO_USE_TEXTURE_MASK;
	uint64_t texture_flags = BO_USE_TEXTURE_MASK;
	bool is_kvm = vm_type() & HYPERTYPE_TYPE_KVM;

	// HW protected buffers also need to be scanned out.
	const uint64_t hw_protected =
	    i915->has_hw_protection ? (BO_USE_PROTECTED | BO_USE_SCANOUT) : 0;

	const uint64_t linear_mask = BO_USE_RENDERSCRIPT | BO_USE_LINEAR | BO_USE_SW_READ_OFTEN |
				     BO_USE_SW_WRITE_OFTEN | BO_USE_SW_READ_RARELY |
				     BO_USE_SW_WRITE_RARELY;

	uint64_t camera_mask = BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE;

	struct format_metadata metadata_linear = { .tiling = I915_TILING_NONE,
						   .priority = 1,
						   .modifier = DRM_FORMAT_MOD_LINEAR };

	drv_add_combinations(drv, scanout_render_formats, ARRAY_SIZE(scanout_render_formats),
			     &metadata_linear, scanout_and_render);

	drv_add_combinations(drv, render_formats, ARRAY_SIZE(render_formats), &metadata_linear,
			     render);

	drv_add_combinations(drv, texture_only_formats, ARRAY_SIZE(texture_only_formats),
			     &metadata_linear, texture_only);

	drv_modify_linear_combinations(drv);

	/* NV12 format for camera, display, decoding and encoding. */
	/* IPU3 camera ISP supports only NV12 output. */
	drv_modify_combination(drv, DRM_FORMAT_NV12, &metadata_linear,
			       BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE | BO_USE_SCANOUT |
				   BO_USE_HW_VIDEO_DECODER | BO_USE_HW_VIDEO_ENCODER |
				   hw_protected);

	/* Android CTS tests require this. */
	drv_add_combination(drv, DRM_FORMAT_BGR888, &metadata_linear, BO_USE_SW_MASK);
	drv_modify_combination(drv, DRM_FORMAT_ABGR2101010, &metadata_linear, BO_USE_SW_MASK);
	drv_add_combination(drv, DRM_FORMAT_RGB888, &metadata_linear, BO_USE_SW_MASK);

	/*
	 * R8 format is used for Android's HAL_PIXEL_FORMAT_BLOB and is used for JPEG snapshots
	 * from camera and input/output from hardware decoder/encoder.
	 */
	drv_modify_combination(drv, DRM_FORMAT_R8, &metadata_linear,
			       BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE | BO_USE_HW_VIDEO_DECODER |
				   BO_USE_HW_VIDEO_ENCODER | BO_USE_GPU_DATA_BUFFER |
				   BO_USE_SENSOR_DIRECT_DATA);
       drv_modify_combination(drv, DRM_FORMAT_ABGR8888, &metadata_linear, BO_USE_CURSOR | BO_USE_SCANOUT);
       drv_modify_combination(drv, DRM_FORMAT_NV12, &metadata_linear,
                              BO_USE_RENDERING | BO_USE_TEXTURE | BO_USE_CAMERA_MASK);
       drv_modify_combination(drv, DRM_FORMAT_YUYV, &metadata_linear,
                              BO_USE_TEXTURE | BO_USE_CAMERA_MASK | BO_USE_RENDERING);
       drv_modify_combination(drv, DRM_FORMAT_VYUY, &metadata_linear,
                              BO_USE_TEXTURE | BO_USE_CAMERA_MASK | BO_USE_RENDERING);
       drv_modify_combination(drv, DRM_FORMAT_UYVY, &metadata_linear,
                              BO_USE_TEXTURE | BO_USE_CAMERA_MASK | BO_USE_RENDERING);
       drv_modify_combination(drv, DRM_FORMAT_YVYU, &metadata_linear,
                              BO_USE_TEXTURE | BO_USE_CAMERA_MASK | BO_USE_RENDERING);
       drv_modify_combination(drv, DRM_FORMAT_YVU420_ANDROID, &metadata_linear,
                              BO_USE_TEXTURE | BO_USE_CAMERA_MASK);

       /* Media/Camera expect these formats support. */
       drv_add_combinations(drv, linear_source_formats, ARRAY_SIZE(linear_source_formats),
                            &metadata_linear, texture_flags | BO_USE_CAMERA_MASK);


	const uint64_t render_not_linear = unset_flags(render, linear_mask | camera_mask);
	uint64_t scanout_and_render_not_linear = render_not_linear | BO_USE_SCANOUT;
	uint64_t texture_flags_video =
           unset_flags(texture_flags, BO_USE_RENDERSCRIPT | BO_USE_SW_WRITE_OFTEN |
                                          BO_USE_SW_READ_OFTEN | BO_USE_LINEAR);

	struct format_metadata metadata_x_tiled = { .tiling = I915_TILING_X,
						    .priority = 2,
						    .modifier = I915_FORMAT_MOD_X_TILED };

	drv_add_combinations(drv, render_formats, ARRAY_SIZE(render_formats), &metadata_x_tiled,
			     render_not_linear);
	drv_add_combinations(drv, scanout_render_formats, ARRAY_SIZE(scanout_render_formats),
			     &metadata_x_tiled, scanout_and_render_not_linear);
	drv_add_combinations(drv, linear_source_formats, ARRAY_SIZE(linear_source_formats),
                             &metadata_x_tiled, texture_flags_video | BO_USE_CAMERA_MASK);

	if (i915_has_tile4(i915)) {
		struct format_metadata metadata_4_tiled = { .tiling = I915_TILING_4,
							    .priority = 3,
							    .modifier = I915_FORMAT_MOD_4_TILED };
/* Support tile4 NV12 and P010 for libva */
#ifdef I915_SCANOUT_4_TILED
		const uint64_t nv12_usage =
		    BO_USE_TEXTURE | BO_USE_HW_VIDEO_DECODER | BO_USE_SCANOUT | hw_protected;
		const uint64_t p010_usage =
		    BO_USE_TEXTURE | BO_USE_HW_VIDEO_DECODER | hw_protected | BO_USE_SCANOUT;
#else
		const uint64_t nv12_usage = BO_USE_TEXTURE | BO_USE_HW_VIDEO_DECODER;
		const uint64_t p010_usage = nv12_usage;
#endif
		drv_add_combination(drv, DRM_FORMAT_NV12, &metadata_4_tiled, nv12_usage);
		drv_add_combination(drv, DRM_FORMAT_P010, &metadata_4_tiled, p010_usage);
		drv_add_combination(drv, DRM_FORMAT_P010_INTEL, &metadata_4_tiled, p010_usage);
		drv_add_combinations(drv, source_formats, ARRAY_SIZE(source_formats), &metadata_4_tiled,
								texture_flags | BO_USE_NON_GPU_HW);

		// in dual gpu case, only alloc x-tiling for dgpu for render
		if (!(drv->gpu_grp_type & GPU_GRP_TYPE_HAS_INTEL_IGPU_BIT) &&
			!(drv->gpu_grp_type & GPU_GRP_TYPE_HAS_VIRTIO_GPU_BLOB_BIT)) {
			drv_add_combinations(drv, render_formats, ARRAY_SIZE(render_formats),
								&metadata_4_tiled, render_not_linear);
			drv_add_combinations(drv, scanout_render_formats,
								ARRAY_SIZE(scanout_render_formats), &metadata_4_tiled,
								render_not_linear);
		}
	} else {
		struct format_metadata metadata_y_tiled = { .tiling = I915_TILING_Y,
							    .priority = 3,
							    .modifier = I915_FORMAT_MOD_Y_TILED };
/* Support y-tiled NV12 and P010 for libva */
#ifdef I915_SCANOUT_Y_TILED
		const uint64_t nv12_usage =
		    BO_USE_TEXTURE | BO_USE_HW_VIDEO_DECODER | BO_USE_SCANOUT | hw_protected;
		const uint64_t p010_usage = BO_USE_TEXTURE | BO_USE_HW_VIDEO_DECODER |
					    hw_protected |
					    (i915->graphics_version >= 11 ? BO_USE_SCANOUT : 0);
#else
		const uint64_t nv12_usage = BO_USE_TEXTURE | BO_USE_HW_VIDEO_DECODER;
		const uint64_t p010_usage = nv12_usage;
#endif
		drv_add_combination(drv, DRM_FORMAT_NV12, &metadata_y_tiled, nv12_usage);
		drv_add_combination(drv, DRM_FORMAT_P010, &metadata_y_tiled, p010_usage);
		drv_add_combination(drv, DRM_FORMAT_P010_INTEL, &metadata_y_tiled, p010_usage);
		drv_add_combinations(drv, source_formats, ARRAY_SIZE(source_formats), &metadata_y_tiled,
				     texture_flags | BO_USE_NON_GPU_HW);

		if (!(drv->gpu_grp_type & GPU_GRP_TYPE_HAS_INTEL_DGPU_BIT) &&
			!(drv->gpu_grp_type & GPU_GRP_TYPE_HAS_VIRTIO_GPU_BLOB_P2P_BIT)) {
			drv_add_combinations(drv, render_formats, ARRAY_SIZE(render_formats),
								&metadata_y_tiled, render_not_linear);
			/* Y-tiled scanout isn't available on old platforms so we add
			* |scanout_render_formats| without that USE flag.
			*/
			drv_add_combinations(drv, scanout_render_formats,
								ARRAY_SIZE(scanout_render_formats), &metadata_y_tiled,
								scanout_and_render_not_linear);
		}
	}
	return 0;
}

static int i915_align_dimensions(struct bo *bo, uint32_t format, uint32_t tiling, uint32_t *stride,
				 uint32_t *aligned_height)
{
	uint32_t horizontal_alignment = 64;
	uint32_t vertical_alignment = 4;
	struct i915_device *i915 = bo->drv->priv;
	if (GEN_VERSION_X10(i915) >= 125) {
		horizontal_alignment = 4;
		vertical_alignment = 4;
	}

	switch (tiling) {
	default:
		break;
	case I915_TILING_NONE:
		/*
		 * The Intel GPU doesn't need any alignment in linear mode,
		 * but libva requires the allocation stride to be aligned to
		 * 16 bytes and height to 4 rows. Further, we round up the
		 * horizontal alignment so that row start on a cache line (64
		 * bytes).
		 */
#ifdef LINEAR_ALIGN_256
		/*
		 * If we want to import these buffers to amdgpu they need to
		 * their match LINEAR_ALIGNED requirement of 256 byte alignement.
		 */
		horizontal_alignment = 256;
#else
		horizontal_alignment = 64;
#endif
		/*
		 * For R8 and height=1, we assume the surface will be used as a linear buffer blob
		 * (such as VkBuffer). The hardware allows vertical_alignment=1 only for non-tiled
		 * 1D surfaces, which covers the VkBuffer case. However, if the app uses the surface
		 * as a 2D image with height=1, then this code is buggy. For 2D images, the hardware
		 * requires a vertical_alignment >= 4, and underallocating with vertical_alignment=1
		 * will cause the GPU to read out-of-bounds.
		 *
		 * TODO: add a new DRM_FORMAT_BLOB format for this case, or further tighten up the
		 * constraints with GPU_DATA_BUFFER usage when the guest has migrated to use
		 * virtgpu_cross_domain backend which passes that flag through.
		 */
		if (format == DRM_FORMAT_R8 && *aligned_height == 1) {
			vertical_alignment = 1;
		} else {
			vertical_alignment = 4;
		}

		break;

	case I915_TILING_X:
		horizontal_alignment = 512;
		vertical_alignment = 8;
		break;

	case I915_TILING_Y:
		horizontal_alignment = 128;
		vertical_alignment = 32;
		break;
	case I915_TILING_4:
		horizontal_alignment = 128;
		vertical_alignment = 32;
		break;
	}

	*aligned_height = ALIGN(*aligned_height, vertical_alignment);

	if(DRM_FORMAT_R8 != bo->meta.format)
	*stride = ALIGN(*stride, horizontal_alignment);

	return 0;
}

static void i915_clflush(void *start, size_t size)
{
	void *p = (void *)(((uintptr_t)start) & ~I915_CACHELINE_MASK);
	void *end = (void *)((uintptr_t)start + size);

	__builtin_ia32_mfence();
	while (p < end) {
#if defined(__CLFLUSHOPT__)
		__builtin_ia32_clflushopt(p);
#else
		__builtin_ia32_clflush(p);
#endif
		p = (void *)((uintptr_t)p + I915_CACHELINE_SIZE);
	}
	__builtin_ia32_mfence();
}

static int gem_param(int fd, int name)
{
	int v = -1; /* No param uses (yet) the sign bit, reserve it for errors */

	struct drm_i915_getparam gp = {.param = name, .value = &v };
	if (drmIoctl(fd, DRM_IOCTL_I915_GETPARAM, &gp))
		return -1;

	return v;
}

static void i915_bo_update_meminfo(struct i915_device *i915_dev,
				   const struct drm_i915_query_memory_regions *meminfo)
{
	i915_dev->has_local_mem = false;
	for (uint32_t i = 0; i < meminfo->num_regions; i++) {
		const struct drm_i915_memory_region_info *mem = &meminfo->regions[i];
		switch (mem->region.memory_class) {
		case I915_MEMORY_CLASS_SYSTEM:
			i915_dev->sys.region = mem->region;
			i915_dev->sys.size = mem->probed_size;
			break;
		case I915_MEMORY_CLASS_DEVICE:
			i915_dev->vram.region = mem->region;
			i915_dev->vram.size = mem->probed_size;
			i915_dev->has_local_mem = i915_dev->vram.size > 0;
			break;
		default:
			break;
		}
	}
}

static void prelim_i915_bo_update_meminfo(struct i915_device *i915_dev,
				   const struct prelim_drm_i915_query_memory_regions *meminfo)
{
	i915_dev->has_local_mem = false;
	for (uint32_t i = 0; i < meminfo->num_regions; i++) {
		const struct prelim_drm_i915_memory_region_info *mem = &meminfo->regions[i];
		switch (mem->region.memory_class) {
		case I915_MEMORY_CLASS_SYSTEM:
			i915_dev->sys.region = mem->region;
			i915_dev->sys.size = mem->probed_size;
			break;
		case I915_MEMORY_CLASS_DEVICE:
			i915_dev->vram.region = mem->region;
			i915_dev->vram.size = mem->probed_size;
			i915_dev->has_local_mem = i915_dev->vram.size > 0;
			break;
		default:
			break;
		}
	}
}

static bool i915_bo_query_prelim_meminfo(struct driver *drv, struct i915_device *i915_dev)
{
	struct drm_i915_query_item item = {
		.query_id = PRELIM_DRM_I915_QUERY_MEMORY_REGIONS,
	};

	struct drm_i915_query query = {
		.num_items = 1, .items_ptr = (uintptr_t)&item,
	};
	int ret = 0;
	ret = drmIoctl(drv->fd, DRM_IOCTL_I915_QUERY, &query);
	if (ret < 0) {
		drv_loge("drv: Failed to query PRELIM_DRM_I915_QUERY_MEMORY_REGIONS\n");
		return false;
	}
	if (item.length <= 0) {
		return false;
	}
	struct prelim_drm_i915_query_memory_regions *meminfo = calloc(1, item.length);
	if (!meminfo) {
		 drv_loge("drv: %s Exit due to memory allocation failure\n", __func__);
		return false;
	}
	item.data_ptr = (uintptr_t)meminfo;
	ret = drmIoctl(drv->fd, DRM_IOCTL_I915_QUERY, &query);
	if (ret < 0 || item.length <= 0) {
		free(meminfo);
		drv_loge("%s:%d DRM_IOCTL_I915_QUERY error\n", __FUNCTION__, __LINE__);
		return false;
	}
	prelim_i915_bo_update_meminfo(i915_dev, meminfo);

	free(meminfo);

	is_prelim_kernel = true;
	return true;
}

static bool i915_bo_query_meminfo(struct driver *drv, struct i915_device *i915_dev)
{
	struct drm_i915_query_item item = {
		.query_id = DRM_I915_QUERY_MEMORY_REGIONS,
	};

	struct drm_i915_query query = {
		.num_items = 1, .items_ptr = (uintptr_t)&item,
	};
	if (drmIoctl(drv->fd, DRM_IOCTL_I915_QUERY, &query)) {
		drv_loge("drv: Failed to DRM_IOCTL_I915_QUERY\n");
		return false;
	}
	struct drm_i915_query_memory_regions *meminfo = calloc(1, item.length);
	if (!meminfo) {
		drv_loge("drv: %s Exit due to memory allocation failure\n", __func__);
		return false;
	}
	item.data_ptr = (uintptr_t)meminfo;
	if (drmIoctl(drv->fd, DRM_IOCTL_I915_QUERY, &query) || item.length <= 0) {
		free(meminfo);
		drv_loge("%s:%d DRM_IOCTL_I915_QUERY error\n", __FUNCTION__, __LINE__);
		return false;
	}
	i915_bo_update_meminfo(i915_dev, meminfo);

	free(meminfo);

	return true;
}

static int i915_init(struct driver *drv)
{
	int ret, val;
	struct i915_device *i915;
	struct intel_gpu_info info;

	i915 = calloc(1, sizeof(*i915));
	if (!i915)
		return -ENOMEM;

	ret = gem_param(drv->fd, I915_PARAM_CHIPSET_ID);
	if (ret == -1) {
		drv_loge("Failed to get I915_PARAM_CHIPSET_ID\n");
		free(i915);
		return -EINVAL;
	}
	i915->device_id = ret;

	/* must call before i915->graphics_version is used anywhere else */
	memset(&info, 0, sizeof(info));
	ret = intel_gpu_info_from_device_id(i915->device_id, &info);
	if (ret != 0) {
		drv_loge("%s: Failed to get device info\n", __func__);
		free(i915);
		return -EINVAL;
	}
	i915->graphics_version = info.graphics_version;
	i915->sub_version = info.sub_version;
	i915->is_xelpd = info.is_xelpd;

	i915_get_modifier_order(i915);

	ret = gem_param(drv->fd, I915_PARAM_HAS_LLC);
	if (ret == -1) {
		drv_loge("Failed to get I915_PARAM_HAS_LLC\n");
		free(i915);
		return -EINVAL;
	}
	i915->has_llc = ret > 0;

	ret = gem_param(drv->fd, I915_PARAM_MMAP_GTT_VERSION);
	if (ret == -1) {
		drv_loge("Failed to get I915_PARAM_MMAP_GTT_VERSION\n");
		free(i915);
		return -EINVAL;
	}
	i915->has_mmap_offset = ret >= 4;

	if (!i915_bo_query_prelim_meminfo(drv, i915)) {
		i915_bo_query_meminfo(drv, i915);
	} else {
		drv_logi("drv: kernel supports prelim\n");
	}

#define FORCE_MEM_PROP "sys.icr.gralloc.force_mem"
	char prop[PROPERTY_VALUE_MAX];
	i915->force_mem_local = (i915->vram.size > 0) &&
				property_get(FORCE_MEM_PROP, prop, "local") > 0 &&
				!strcmp(prop, "local");
	if (i915->force_mem_local) {
		drv_logi("Force to use local memory");
	}

	if (i915->graphics_version >= 12)
		i915->has_hw_protection = true;

	uint64_t width = 0, height = 0;
	if (drmGetCap(drv->fd, DRM_CAP_CURSOR_WIDTH, &width)) {
		drv_logi("cannot get cursor width. \n");
	} else if (drmGetCap(drv->fd, DRM_CAP_CURSOR_HEIGHT, &height)) {
		drv_logi("cannot get cursor height. \n");
	}

	if (!width)
		width = kDefaultCursorWidth;
	i915->cursor_width = width;

	if (!height)
		height = kDefaultCursorHeight;
	i915->cursor_height = height;

	drv->priv = i915;
	return i915_add_combinations(drv);
}

/*
 * Returns true if the height of a buffer of the given format should be aligned
 * to the largest coded unit (LCU) assuming that it will be used for video. This
 * is based on gmmlib's GmmIsYUVFormatLCUAligned().
 */
static bool i915_format_needs_LCU_alignment(uint32_t format, size_t plane,
					    const struct i915_device *i915)
{
	switch (format) {
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_P010:
	case DRM_FORMAT_P016:
		return (i915->graphics_version == 11 || i915->graphics_version == 12) && plane == 1;
	}
	return false;
}

static int i915_bo_from_format(struct bo *bo, uint32_t width, uint32_t height, uint32_t format)
{
	uint32_t offset;
	size_t plane;
	int ret, pagesize;
	struct i915_device *i915 = bo->drv->priv;

	offset = 0;
	pagesize = getpagesize();

	for (plane = 0; plane < drv_num_planes_from_format(format); plane++) {
		uint32_t stride = drv_stride_from_format(format, width, plane);
		uint32_t plane_height = drv_height_from_format(format, height, plane);

		if (bo->meta.tiling != I915_TILING_NONE)
			assert(IS_ALIGNED(offset, pagesize));

		ret = i915_align_dimensions(bo, format, bo->meta.tiling, &stride, &plane_height);
		if (ret)
			return ret;

		if (i915_format_needs_LCU_alignment(format, plane, i915)) {
			/*
			 * Align the height of the V plane for certain formats to the
			 * largest coded unit (assuming that this BO may be used for video)
			 * to be consistent with gmmlib.
			 */
			plane_height = ALIGN(plane_height, 64);
		}

		bo->meta.strides[plane] = stride;
		bo->meta.sizes[plane] = stride * plane_height;
		bo->meta.offsets[plane] = offset;
		offset += bo->meta.sizes[plane];
	}

	bo->meta.total_size = ALIGN(offset, pagesize);

	return 0;
}

static size_t i915_num_planes_from_modifier(struct driver *drv, uint32_t format, uint64_t modifier)
{
	size_t num_planes = drv_num_planes_from_format(format);
	if (modifier == I915_FORMAT_MOD_Y_TILED_CCS ||
	    modifier == I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS) {
		assert(num_planes == 1);
		return 2;
	}

	return num_planes;
}

static int i915_bo_compute_metadata(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
				    uint64_t use_flags, const uint64_t *modifiers, uint32_t count)
{
	uint64_t modifier;
	struct i915_device *i915 = bo->drv->priv;
	bool huge_bo = (i915->graphics_version < 11) && (width > 4096);

	if (modifiers) {
		modifier =
		    drv_pick_modifier(modifiers, count, i915->modifier.order, i915->modifier.count);
	} else {
		struct combination *combo = drv_get_combination(bo->drv, format, use_flags);
		if (!combo)
			return -EINVAL;
		modifier = combo->metadata.modifier;
	}

	/*
	 * i915 only supports linear/x-tiled above 4096 wide on Gen9/Gen10 GPU.
	 * VAAPI decode in NV12 Y tiled format so skip modifier change for NV12/P010 huge bo.
	 */
	if (huge_bo && format != DRM_FORMAT_NV12 && format != DRM_FORMAT_P010 &&
	    modifier != I915_FORMAT_MOD_X_TILED && modifier != DRM_FORMAT_MOD_LINEAR) {
		uint32_t i;
		for (i = 0; modifiers && i < count; i++) {
			if (modifiers[i] == I915_FORMAT_MOD_X_TILED)
				break;
		}
		if (i == count)
			modifier = DRM_FORMAT_MOD_LINEAR;
		else
			modifier = I915_FORMAT_MOD_X_TILED;
	}

	/*
	 * Skip I915_FORMAT_MOD_Y_TILED_CCS modifier if compression is disabled
	 * Pick y tiled modifier if it has been passed in, otherwise use linear
	 */
	if (!bo->drv->compression && modifier == I915_FORMAT_MOD_Y_TILED_CCS) {
		uint32_t i;
		for (i = 0; modifiers && i < count; i++) {
			if (modifiers[i] == I915_FORMAT_MOD_Y_TILED)
				break;
		}
		if (i == count)
			modifier = DRM_FORMAT_MOD_LINEAR;
		else
			modifier = I915_FORMAT_MOD_Y_TILED;
	}

	/* Prevent gen 8 and earlier from trying to use a tiling modifier */
	if (i915->graphics_version <= 8 && format == DRM_FORMAT_ARGB8888) {
		modifier = DRM_FORMAT_MOD_LINEAR;
	}

	switch (modifier) {
	case DRM_FORMAT_MOD_LINEAR:
		bo->meta.tiling = I915_TILING_NONE;
		break;
	case I915_FORMAT_MOD_X_TILED:
		bo->meta.tiling = I915_TILING_X;
		break;
	case I915_FORMAT_MOD_Y_TILED:
	case I915_FORMAT_MOD_Y_TILED_CCS:
	case I915_FORMAT_MOD_Yf_TILED:
	case I915_FORMAT_MOD_Yf_TILED_CCS:

	/* For now support only I915_TILING_Y as this works with all
	 * IPs(render/media/display)
	 */
	case I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS:
		bo->meta.tiling = I915_TILING_Y;
		break;
	case I915_FORMAT_MOD_4_TILED:
		bo->meta.tiling = I915_TILING_4;
		break;
	}

	bo->meta.format_modifier = modifier;

	if (use_flags & BO_USE_SCANOUT) {
		drv_logd("Use tiling mode %s for scan-out buffer, modifier=0x%lx\n",
			 tiling_to_string(bo->meta.tiling), modifier);
	}

	if (format == DRM_FORMAT_YVU420_ANDROID) {
		/*
		 * We only need to be able to use this as a linear texture,
		 * which doesn't put any HW restrictions on how we lay it
		 * out. The Android format does require the stride to be a
		 * multiple of 16 and expects the Cr and Cb stride to be
		 * ALIGN(Y_stride / 2, 16), which we can make happen by
		 * aligning to 32 bytes here.
		 */
		uint32_t stride = ALIGN(width, 32);
		return drv_bo_from_format(bo, stride, 1, height, format);
	} else if (modifier == I915_FORMAT_MOD_Y_TILED_CCS) {
		/*
		 * For compressed surfaces, we need a color control surface
		 * (CCS). Color compression is only supported for Y tiled
		 * surfaces, and for each 32x16 tiles in the main surface we
		 * need a tile in the control surface.  Y tiles are 128 bytes
		 * wide and 32 lines tall and we use that to first compute the
		 * width and height in tiles of the main surface. stride and
		 * height are already multiples of 128 and 32, respectively:
		 */
		uint32_t stride = drv_stride_from_format(format, width, 0);
		uint32_t width_in_tiles = DIV_ROUND_UP(stride, 128);
		uint32_t height_in_tiles = DIV_ROUND_UP(height, 32);
		uint32_t size = width_in_tiles * height_in_tiles * 4096;
		uint32_t offset = 0;

		bo->meta.strides[0] = width_in_tiles * 128;
		bo->meta.sizes[0] = size;
		bo->meta.offsets[0] = offset;
		offset += size;

		/*
		 * Now, compute the width and height in tiles of the control
		 * surface by dividing and rounding up.
		 */
		uint32_t ccs_width_in_tiles = DIV_ROUND_UP(width_in_tiles, 32);
		uint32_t ccs_height_in_tiles = DIV_ROUND_UP(height_in_tiles, 16);
		uint32_t ccs_size = ccs_width_in_tiles * ccs_height_in_tiles * 4096;

		/*
		 * With stride and height aligned to y tiles, offset is
		 * already a multiple of 4096, which is the required alignment
		 * of the CCS.
		 */
		bo->meta.strides[1] = ccs_width_in_tiles * 128;
		bo->meta.sizes[1] = ccs_size;
		bo->meta.offsets[1] = offset;
		offset += ccs_size;

		bo->meta.num_planes = i915_num_planes_from_modifier(bo->drv, format, modifier);
		bo->meta.total_size = offset;
	} else if (modifier == I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS) {

		/*
		 * considering only 128 byte compression and one cache line of
		 * aux buffer(64B) contains compression status of 4-Y tiles.
		 * Which is 4 * (128B * 32L).
		 * line stride(bytes) is 4 * 128B
		 * and tile stride(lines) is 32L
		 */
		uint32_t stride = ALIGN(drv_stride_from_format(format, width, 0), 512);

		height = ALIGN(drv_height_from_format(format, height, 0), 32);

		if (i915->is_xelpd && (stride > 1)) {
			stride = 1 << (32 - __builtin_clz(stride - 1));
			height = ALIGN(drv_height_from_format(format, height, 0), 128);
		}

		bo->meta.strides[0] = stride;
		/* size calculation and alignment are 64KB aligned
		 * size as per spec
		 */
		bo->meta.sizes[0] = ALIGN(stride * height, 65536);
		bo->meta.offsets[0] = 0;

		/* Aux buffer is linear and page aligned. It is placed after
		 * other planes and aligned to main buffer stride.
		 */
		bo->meta.strides[1] = bo->meta.strides[0] / 8;
		/* Aligned to page size */
		bo->meta.sizes[1] = ALIGN(bo->meta.sizes[0] / 256, getpagesize());
		bo->meta.offsets[1] = bo->meta.sizes[0];
		/* Total number of planes & sizes */
		bo->meta.num_planes = i915_num_planes_from_modifier(bo->drv, format, modifier);
		bo->meta.total_size = bo->meta.sizes[0] + bo->meta.sizes[1];
	} else {
		return i915_bo_from_format(bo, width, height, format);
	}
	return 0;
}

static bool is_need_local(int64_t use_flags, uint64_t gpu_grp_type)
{
	static bool local = false;

	if (use_flags & BO_USE_LOCAL_MEMORY) {
		local = true;
	}

	if (use_flags & BO_USE_SW_READ_RARELY || use_flags & BO_USE_SW_READ_OFTEN ||
	    use_flags & BO_USE_SW_WRITE_RARELY || use_flags & BO_USE_SW_WRITE_OFTEN) {
		local = false;
	}

	//dGPU only use the local memory
	if ((gpu_grp_type & GPU_GRP_TYPE_HAS_INTEL_DGPU_BIT) && !(gpu_grp_type & GPU_GRP_TYPE_HAS_INTEL_IGPU_BIT)) {
		local = true;
	}
	return local;
}

static inline void
intel_gem_add_ext(__u64 *ptr, uint32_t ext_name,
		struct i915_user_extension *ext) {
	__u64 *iter = ptr;
	while (*iter != 0) {
		iter = (__u64 *) &((struct i915_user_extension *)(uintptr_t)*iter)->next_extension;
	}
	ext->name = ext_name;
	*iter = (uintptr_t) ext;
}

static int i915_bo_create_from_metadata(struct bo *bo)
{
	int ret;
	size_t plane;
	uint32_t gem_handle;
	struct drm_i915_gem_set_tiling gem_set_tiling = { 0 };
	struct i915_device *i915 = bo->drv->priv;
	int64_t use_flags = bo->meta.use_flags;
	bool local = is_need_local(use_flags, bo->drv->gpu_grp_type);

	if (local && i915->has_local_mem) {
		if (!is_prelim_kernel) {
			/* All new BOs we get from the kernel are zeroed, so we don't need to
			 * worry about that here.
			 */
			struct drm_i915_gem_create_ext gem_create_ext = {
				.size = ALIGN(bo->meta.total_size, 0x10000),
			};

			struct drm_i915_gem_memory_class_instance regions[2];

			struct drm_i915_gem_create_ext_memory_regions ext_regions = {
				.base = { .name = I915_GEM_CREATE_EXT_MEMORY_REGIONS },
				.num_regions = 0,
				.regions = (uintptr_t)regions,
			};
			enum iris_heap heap = flags_to_heap(i915, use_flags);
			switch (heap) {
				case IRIS_HEAP_DEVICE_LOCAL_PREFERRED:
					/* For vram allocations, still use system memory as a fallback. */
					regions[ext_regions.num_regions++] = i915->vram.region;
					regions[ext_regions.num_regions++] = i915->sys.region;
					break;
				case IRIS_HEAP_DEVICE_LOCAL:
					regions[ext_regions.num_regions++] = i915->vram.region;
					break;
				case IRIS_HEAP_SYSTEM_MEMORY:
					regions[ext_regions.num_regions++] = i915->sys.region;
					break;
				case IRIS_HEAP_MAX:
					break;
			}
			intel_gem_add_ext(&gem_create_ext.extensions,
					I915_GEM_CREATE_EXT_MEMORY_REGIONS,
					&ext_regions.base);

			if (heap == IRIS_HEAP_DEVICE_LOCAL_PREFERRED) {
				gem_create_ext.flags |= I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS;
			}
			/* It should be safe to use GEM_CREATE_EXT without checking, since we are
			 * in the side of the branch where discrete memory is available. So we
			 * can assume GEM_CREATE_EXT is supported already.
			 */
			ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_CREATE_EXT, &gem_create_ext);
			if (ret) {
				drv_loge("drv: DRM_IOCTL_I915_GEM_CREATE_EXT failed (size=%llu)\n",
					gem_create_ext.size);
				return -errno;
			} else {
				drv_loge("drv: DRM_IOCTL_I915_GEM_CREATE_EXT OK (size=%llu)\n",
					gem_create_ext.size);
			}
			gem_handle = gem_create_ext.handle;

		} else {
			struct prelim_drm_i915_gem_memory_class_instance regions[2];
			uint32_t nregions = 0;
			if (i915->force_mem_local) {
				/* For vram allocations, still use system memory as a fallback. */
				regions[nregions++] = i915->vram.region;
				regions[nregions++] = i915->sys.region;
			} else {
				regions[nregions++] = i915->sys.region;
			}

			struct prelim_drm_i915_gem_object_param region_param = {
				.size = nregions,
				.data = (uintptr_t)regions,
				.param = PRELIM_I915_OBJECT_PARAM | PRELIM_I915_PARAM_MEMORY_REGIONS,
			};

			struct prelim_drm_i915_gem_create_ext_setparam setparam_region = {
				.base = { .name = PRELIM_I915_GEM_CREATE_EXT_SETPARAM },
				.param = region_param,
			};

			struct prelim_drm_i915_gem_create_ext gem_create_ext = {
				.size = ALIGN(bo->meta.total_size, 0x10000),
				.extensions = (uintptr_t)&setparam_region,
			};
			/* It should be safe to use GEM_CREATE_EXT without checking, since we are
			 * in the side of the branch where discrete memory is available. So we
			 * can assume GEM_CREATE_EXT is supported already.
			 */
			ret = drmIoctl(bo->drv->fd, PRELIM_DRM_IOCTL_I915_GEM_CREATE_EXT, &gem_create_ext);
			if (ret) {
				drv_loge("drv: PRELIM_DRM_IOCTL_I915_GEM_CREATE_EXT failed (size=%llu)\n",
					gem_create_ext.size);
				return -errno;
			}
			gem_handle = gem_create_ext.handle;
		}
	} else {
		struct drm_i915_gem_create gem_create;
		memset(&gem_create, 0, sizeof(gem_create));
		gem_create.size = bo->meta.total_size;
		ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_CREATE, &gem_create);
		if (ret) {
			drv_loge("DRM_IOCTL_I915_GEM_CREATE failed (size=%llu)\n", gem_create.size);
			return -errno;
		}
		gem_handle = gem_create.handle;
	}

	for (plane = 0; plane < bo->meta.num_planes; plane++)
		bo->handles[plane].u32 = gem_handle;

	/* Set/Get tiling ioctl not supported  based on fence availability
	   Refer : "https://patchwork.freedesktop.org/patch/325343/"
	 */
	if ((GEN_VERSION_X10(i915) != 125) && (i915->graphics_version != 14)) {
		gem_set_tiling.handle = bo->handles[0].u32;
		gem_set_tiling.tiling_mode = bo->meta.tiling;
		gem_set_tiling.stride = bo->meta.strides[0];

		ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_SET_TILING, &gem_set_tiling);
		if (ret) {
			struct drm_gem_close gem_close = { 0 };
			gem_close.handle = bo->handles[0].u32;
			drmIoctl(bo->drv->fd, DRM_IOCTL_GEM_CLOSE, &gem_close);

			drv_loge("DRM_IOCTL_I915_GEM_SET_TILING failed with %d\n", errno);
			return -errno;
		}
	}
	return 0;
}

static void i915_close(struct driver *drv)
{
	free(drv->priv);
	drv->priv = NULL;
}

static int i915_bo_import(struct bo *bo, struct drv_import_fd_data *data)
{
	int ret;
	struct drm_i915_gem_get_tiling gem_get_tiling = { 0 };
	struct i915_device *i915 = bo->drv->priv;

	bo->meta.num_planes =
	    i915_num_planes_from_modifier(bo->drv, data->format, data->format_modifier);

	ret = drv_prime_bo_import(bo, data);
	if (ret)
		return ret;

	/* Set/Get tiling ioctl not supported  based on fence availability
	   Refer : "https://patchwork.freedesktop.org/patch/325343/"
	 */
	if ((GEN_VERSION_X10(i915) != 125) && (i915->graphics_version != 14)) {
		/* TODO(gsingh): export modifiers and get rid of backdoor tiling. */
		gem_get_tiling.handle = bo->handles[0].u32;

		ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_GET_TILING, &gem_get_tiling);
		if (ret) {
			drv_gem_bo_destroy(bo);
			drv_loge("DRM_IOCTL_I915_GEM_GET_TILING failed.\n");
			return ret;
		}
		bo->meta.tiling = gem_get_tiling.tiling_mode;
	} else {
		bo->meta.tiling = data->tiling;
	}
	return 0;
}

static void *i915_bo_map(struct bo *bo, struct vma *vma, uint32_t map_flags)
{
	int ret;
	void *addr = MAP_FAILED;
	struct i915_device *i915 = bo->drv->priv;

	if ((bo->meta.format_modifier == I915_FORMAT_MOD_Y_TILED_CCS) ||
	    (bo->meta.format_modifier == I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS))
		return MAP_FAILED;

	if (i915->has_mmap_offset) {
		struct drm_i915_gem_mmap_offset mmap_arg = {
			.handle = bo->handles[0].u32,
		};

		if (i915->has_local_mem) {
			mmap_arg.flags = I915_MMAP_OFFSET_FIXED;
		} else {
			if ((bo->meta.use_flags & BO_USE_SCANOUT) &&
				!(bo->meta.use_flags &
					(BO_USE_RENDERSCRIPT | BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE | BO_USE_SW_READ_OFTEN))) {
					mmap_arg.flags = I915_MMAP_OFFSET_WC;
			} else {
					mmap_arg.flags = I915_MMAP_OFFSET_WB;
			}
		}

		/* Get the fake offset back */
		int ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_MMAP_OFFSET, &mmap_arg);
		if (ret != 0 && mmap_arg.flags == I915_MMAP_OFFSET_FIXED) {
			if ((bo->meta.use_flags & BO_USE_SCANOUT) &&
			    !(bo->meta.use_flags &
			      (BO_USE_RENDERSCRIPT | BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE | BO_USE_SW_READ_OFTEN))) {
				mmap_arg.flags = I915_MMAP_OFFSET_WC;
			} else {
				mmap_arg.flags = I915_MMAP_OFFSET_WB;
			}

			ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_MMAP_OFFSET, &mmap_arg);
		}

		if (ret != 0) {
			drv_loge("drv: DRM_IOCTL_I915_GEM_MMAP_OFFSET failed ret=%d, errno=0x%x\n",
				ret, errno);
			return MAP_FAILED;
		}

		/* And map it */
		addr = mmap(0, bo->meta.total_size, PROT_READ | PROT_WRITE, MAP_SHARED, bo->drv->fd,
			    mmap_arg.offset);
	} else if (bo->meta.tiling == I915_TILING_NONE) {
		struct drm_i915_gem_mmap gem_map = { 0 };
		/* TODO(b/118799155): We don't seem to have a good way to
			* detect the use cases for which WC mapping is really needed.
			* The current heuristic seems overly coarse and may be slowing
			* down some other use cases unnecessarily.
			*
			* For now, care must be taken not to use WC mappings for
			* Renderscript and camera use cases, as they're
			* performance-sensitive. */
		if ((bo->meta.use_flags & BO_USE_SCANOUT) &&
			!(bo->meta.use_flags &
				(BO_USE_RENDERSCRIPT | BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE | BO_USE_SW_READ_OFTEN)))
			gem_map.flags = I915_MMAP_WC;

		gem_map.handle = bo->handles[0].u32;
		gem_map.offset = 0;
		gem_map.size = bo->meta.total_size;

		ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_MMAP, &gem_map);
		/* DRM_IOCTL_I915_GEM_MMAP mmaps the underlying shm
			* file and returns a user space address directly, ie,
			* doesn't go through mmap. If we try that on a
			* dma-buf that doesn't have a shm file, i915.ko
			* returns ENXIO.  Fall through to
			* DRM_IOCTL_I915_GEM_MMAP_GTT in that case, which
			* will mmap on the drm fd instead. */
		if (ret == 0)
			addr = (void *)(uintptr_t)gem_map.addr_ptr;
	}else {
		struct drm_i915_gem_mmap_gtt gem_map = { 0 };

		gem_map.handle = bo->handles[0].u32;
		ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_MMAP_GTT, &gem_map);
		if (ret) {
			struct drm_i915_gem_mmap gem_map;
			memset(&gem_map, 0, sizeof(gem_map));

			if ((bo->meta.use_flags & BO_USE_SCANOUT) &&
			    !(bo->meta.use_flags &
			      (BO_USE_RENDERSCRIPT | BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE | BO_USE_SW_READ_OFTEN)))
				gem_map.flags = I915_MMAP_WC;
			gem_map.handle = bo->handles[0].u32;
			gem_map.offset = 0;
			gem_map.size = bo->meta.total_size;
			ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_MMAP, &gem_map);

			if (ret) {
			        drv_loge("DRM_IOCTL_I915_GEM_MMAP failed\n");
			        return MAP_FAILED;
			}
			addr = (void *)(uintptr_t)gem_map.addr_ptr;

			vma->length = bo->meta.total_size;
			return addr;
		}

		addr = mmap(0, bo->meta.total_size, drv_get_prot(map_flags), MAP_SHARED,
			    bo->drv->fd, gem_map.offset);
	}

	if (addr == MAP_FAILED) {
		drv_loge("i915 GEM mmap failed\n");
		return addr;
	}

	vma->length = bo->meta.total_size;
	return addr;
}

static int i915_bo_invalidate(struct bo *bo, struct mapping *mapping)
{
	int ret;
	struct drm_i915_gem_set_domain set_domain = { 0 };
	struct i915_device *i915_dev = (struct i915_device *)bo->drv->priv;

	if (GEN_VERSION_X10(i915_dev) != 125) {
		set_domain.handle = bo->handles[0].u32;
		if (bo->meta.tiling == I915_TILING_NONE) {
			set_domain.read_domains = I915_GEM_DOMAIN_CPU;
			if (mapping->vma->map_flags & BO_MAP_WRITE)
				set_domain.write_domain = I915_GEM_DOMAIN_CPU;
		} else {
			set_domain.read_domains = I915_GEM_DOMAIN_GTT;
			if (mapping->vma->map_flags & BO_MAP_WRITE)
				set_domain.write_domain = I915_GEM_DOMAIN_GTT;
		}

		ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_SET_DOMAIN, &set_domain);
		if (ret) {
			drv_loge("DRM_IOCTL_I915_GEM_SET_DOMAIN with %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static int i915_bo_flush(struct bo *bo, struct mapping *mapping)
{
	struct i915_device *i915 = bo->drv->priv;
	if (!i915->has_llc
		&& (bo->meta.tiling == I915_TILING_NONE)
		&& (bo->meta.use_flags & BO_USE_SW_WRITE_OFTEN))
		i915_clflush(mapping->vma->addr, mapping->vma->length);

	return 0;
}

static bool i915_is_feature_supported(struct driver *drv, uint64_t feature)
{
	struct i915_device *i915 = drv->priv;
	switch (feature) {
	case DRIVER_DEVICE_FEATURE_I915_DGPU:
		return i915->has_local_mem;
	default:
		return false;
	}
	return false;
}

const struct backend backend_i915 = {
	.name = "i915",
	.init = i915_init,
	.close = i915_close,
	.bo_compute_metadata = i915_bo_compute_metadata,
	.bo_create_from_metadata = i915_bo_create_from_metadata,
	.bo_destroy = drv_gem_bo_destroy,
	.bo_import = i915_bo_import,
	.bo_map = i915_bo_map,
	.bo_unmap = drv_bo_munmap,
	.bo_invalidate = i915_bo_invalidate,
	.bo_flush = i915_bo_flush,
	.resolve_format_and_use_flags = drv_resolve_format_and_use_flags_helper,
	.num_planes_from_modifier = i915_num_planes_from_modifier,
	.is_feature_supported = i915_is_feature_supported,
};

#endif
