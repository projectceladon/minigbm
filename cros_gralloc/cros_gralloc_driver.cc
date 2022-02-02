/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "cros_gralloc_driver.h"

#include <cstdlib>
#include <fcntl.h>
#include <hardware/gralloc.h>
#include <sys/mman.h>
#include <syscall.h>
#include <xf86drm.h>

#include "../util.h"

// Constants taken from pipe_loader_drm.c in Mesa

#define DRM_NUM_NODES 63

// DRM Render nodes start at 128
#define DRM_RENDER_NODE_START 128

// DRM Card nodes start at 0
#define DRM_CARD_NODE_START 0

int memfd_create_wrapper(const char *name, unsigned int flags)
{
	int fd;

#if defined(HAVE_MEMFD_CREATE)
	fd = memfd_create(name, flags);
#elif defined(__NR_memfd_create)
	fd = syscall(__NR_memfd_create, name, flags);
#else
	drv_log("Failed to create memfd '%s': memfd_create not available.", name);
	return -1;
#endif

	if (fd == -1)
		drv_log("Failed to create memfd '%s': %s.\n", name, strerror(errno));

	return fd;
}

cros_gralloc_driver *cros_gralloc_driver::get_instance()
{
	static cros_gralloc_driver s_instance;

	if (!s_instance.is_initialized()) {
		drv_log("Failed to initialize driver.\n");
		return nullptr;
	}

	return &s_instance;
}

static struct driver *init_try_node(int idx, char const *str)
{
	int fd;
	char *node;
	struct driver *drv;

	if (asprintf(&node, str, DRM_DIR_NAME, idx) < 0)
		return NULL;

	fd = open(node, O_RDWR, 0);
	free(node);

	if (fd < 0)
		return NULL;

	drv = drv_create(fd);
	if (!drv)
		close(fd);

	return drv;
}

cros_gralloc_driver::cros_gralloc_driver()
{
	/*
	 * Create a driver from render nodes first, then try card
	 * nodes.
	 *
	 * TODO(gsingh): Enable render nodes on udl/evdi.
	 */

	char const *render_nodes_fmt = "%s/renderD%d";
	char const *card_nodes_fmt = "%s/card%d";
	uint32_t num_nodes = DRM_NUM_NODES;
	uint32_t min_render_node = DRM_RENDER_NODE_START;
	uint32_t max_render_node = (min_render_node + num_nodes);
	uint32_t min_card_node = DRM_CARD_NODE_START;
	uint32_t max_card_node = (min_card_node + num_nodes);

	// Try render nodes...
	for (uint32_t i = min_render_node; i < max_render_node; i++) {
		drv_ = init_try_node(i, render_nodes_fmt);
		if (drv_)
			return;
	}

	// Try card nodes... for vkms mostly.
	for (uint32_t i = min_card_node; i < max_card_node; i++) {
		drv_ = init_try_node(i, card_nodes_fmt);
		if (drv_)
			return;
	}
}

cros_gralloc_driver::~cros_gralloc_driver()
{
	buffers_.clear();

	if (drv_) {
		int fd = drv_get_fd(drv_);
		drv_destroy(drv_);
		drv_ = nullptr;
		close(fd);
	}
}

bool cros_gralloc_driver::is_initialized()
{
	return drv_ != nullptr;
}

bool cros_gralloc_driver::get_resolved_format_and_use_flags(
    const struct cros_gralloc_buffer_descriptor *descriptor, uint32_t *out_format,
    uint64_t *out_use_flags)
{
	uint32_t resolved_format;
	uint64_t resolved_use_flags;
	struct combination *combo;

	drv_resolve_format_and_use_flags(drv_, descriptor->drm_format, descriptor->use_flags,
					 &resolved_format, &resolved_use_flags);

	combo = drv_get_combination(drv_, resolved_format, resolved_use_flags);
	if (!combo && (descriptor->droid_usage & GRALLOC_USAGE_HW_VIDEO_ENCODER) &&
	    descriptor->droid_format != HAL_PIXEL_FORMAT_YCbCr_420_888) {
		// Unmask BO_USE_HW_VIDEO_ENCODER for other formats. They are mostly
		// intermediate formats not passed directly to the encoder (e.g.
		// camera). YV12 is passed to the encoder component, but it is converted
		// to YCbCr_420_888 before being passed to the hw encoder.
		resolved_use_flags &= ~BO_USE_HW_VIDEO_ENCODER;
		combo = drv_get_combination(drv_, resolved_format, resolved_use_flags);
	}
	if (!combo && (descriptor->droid_usage & BUFFER_USAGE_FRONT_RENDERING)) {
		resolved_use_flags &= ~BO_USE_FRONT_RENDERING;
		resolved_use_flags |= BO_USE_LINEAR;
		combo = drv_get_combination(drv_, resolved_format, resolved_use_flags);
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
	uint32_t max_texture_size = drv_get_max_texture_2d_size(drv_);
	if (!get_resolved_format_and_use_flags(descriptor, &resolved_format, &resolved_use_flags))
		return false;

	// Allow blob buffers to go beyond the limit.
	if (descriptor->droid_format == HAL_PIXEL_FORMAT_BLOB)
		return true;

	return descriptor->width <= max_texture_size && descriptor->height <= max_texture_size;
}

int32_t create_reserved_region(const std::string &buffer_name, uint64_t reserved_region_size)
{
	std::string reserved_region_name = buffer_name + " reserved region";

	int32_t reserved_region_fd = memfd_create_wrapper(reserved_region_name.c_str(), FD_CLOEXEC);
	if (reserved_region_fd == -1)
		return -errno;

	if (ftruncate(reserved_region_fd, reserved_region_size)) {
		drv_log("Failed to set reserved region size: %s.\n", strerror(errno));
		return -errno;
	}

	return reserved_region_fd;
}

int32_t cros_gralloc_driver::allocate(const struct cros_gralloc_buffer_descriptor *descriptor,
				      native_handle_t **out_handle)
{
	int ret = 0;
	size_t num_planes;
	size_t num_fds;
	size_t num_ints;
	size_t num_bytes;
	uint32_t resolved_format;
	uint32_t bytes_per_pixel;
	uint64_t resolved_use_flags;
	char *name;
	struct bo *bo;
	struct cros_gralloc_handle *hnd;
	cros_gralloc_buffer *buffer;

	if (!get_resolved_format_and_use_flags(descriptor, &resolved_format, &resolved_use_flags)) {
		drv_log("Failed to resolve format and use_flags.\n");
		return -EINVAL;
	}

	bo = drv_bo_create(drv_, descriptor->width, descriptor->height, resolved_format,
			   resolved_use_flags);
	if (!bo) {
		drv_log("Failed to create bo.\n");
		return -errno;
	}

	/*
	 * If there is a desire for more than one kernel buffer, this can be
	 * removed once the ArcCodec and Wayland service have the ability to
	 * send more than one fd. GL/Vulkan drivers may also have to modified.
	 */
	if (drv_num_buffers_per_bo(bo) != 1) {
		drv_log("Can only support one buffer per bo.\n");
		goto destroy_bo;
	}

	num_planes = drv_bo_get_num_planes(bo);
	num_fds = num_planes;

	if (descriptor->reserved_region_size > 0)
		num_fds += 1;

	num_bytes = sizeof(struct cros_gralloc_handle);
	num_bytes += (descriptor->name.size() + 1);
	/*
	 * Ensure that the total number of bytes is a multiple of sizeof(int) as
	 * native_handle_clone() copies data based on hnd->base.numInts.
	 */
	num_bytes = ALIGN(num_bytes, sizeof(int));
	num_ints = ((num_bytes - sizeof(native_handle_t)) / sizeof(int)) - num_fds;

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
	hnd->droid_format = descriptor->droid_format;
	hnd->usage = descriptor->droid_usage;
	hnd->total_size = descriptor->reserved_region_size + drv_bo_get_total_size(bo);
	hnd->name_offset = handle_data_size;

	name = (char *)(&hnd->data[hnd->name_offset]);
	snprintf(name, descriptor->name.size() + 1, "%s", descriptor->name.c_str());

	buffer = cros_gralloc_buffer::create(bo, hnd);
	if (!buffer) {
		drv_log("Failed to allocate: failed to create cros_gralloc_buffer.\n");
		ret = -1;
		goto destroy_hnd;
	}

	{
		std::lock_guard<std::mutex> lock(mutex_);
		buffers_.emplace(hnd->id, buffer);
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

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		drv_log("Invalid handle.\n");
		return -EINVAL;
	}

	uint32_t id = hnd->id;

	auto buffer_it = buffers_.find(id);
	if (buffer_it != buffers_.end()) {
		auto buffer = buffer_it->second;
		buffer->increase_refcount();
		return 0;
	}

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

	struct bo *bo = drv_bo_import(drv_, &data);
	if (!bo)
		return -EFAULT;

	auto buffer = cros_gralloc_buffer::create(bo, hnd);
	if (!buffer) {
		drv_log("Failed to import: failed to create cros_gralloc_buffer.\n");
		return -1;
	}

	buffers_.emplace(id, buffer);
	return 0;
}

int32_t cros_gralloc_driver::release(buffer_handle_t handle)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		drv_log("Invalid handle.\n");
		return -EINVAL;
	}

	auto buffer_it = buffers_.find(hnd->id);
	if (buffer_it == buffers_.end()) {
		drv_log("Invalid Reference.\n");
		return -EINVAL;
	}

	auto buffer = buffer_it->second;
	if (buffer->decrease_refcount() == 0) {
		buffers_.erase(buffer_it);
		delete buffer;
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
		drv_log("Invalid handle.\n");
		return -EINVAL;
	}

	auto buffer_it = buffers_.find(hnd->id);
	if (buffer_it == buffers_.end()) {
		drv_log("Invalid Reference.\n");
		return -EINVAL;
	}

	auto buffer = buffer_it->second;
	return buffer->lock(rect, map_flags, addr);
}

int32_t cros_gralloc_driver::unlock(buffer_handle_t handle, int32_t *release_fence)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		drv_log("Invalid handle.\n");
		return -EINVAL;
	}

	auto buffer_it = buffers_.find(hnd->id);
	if (buffer_it == buffers_.end()) {
		drv_log("Invalid Reference.\n");
		return -EINVAL;
	}

	auto buffer = buffer_it->second;

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
		drv_log("Invalid handle.\n");
		return -EINVAL;
	}

	auto buffer_it = buffers_.find(hnd->id);
	if (buffer_it == buffers_.end()) {
		drv_log("Invalid Reference.\n");
		return -EINVAL;
	}

	auto buffer = buffer_it->second;
	return buffer->invalidate();
}

int32_t cros_gralloc_driver::flush(buffer_handle_t handle, int32_t *release_fence)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		drv_log("Invalid handle.\n");
		return -EINVAL;
	}

	auto buffer_it = buffers_.find(hnd->id);
	if (buffer_it == buffers_.end()) {
		drv_log("Invalid Reference.\n");
		return -EINVAL;
	}

	auto buffer = buffer_it->second;

	/*
	 * From the ANativeWindow::dequeueBuffer documentation:
	 *
	 * "A value of -1 indicates that the caller may access the buffer immediately without
	 * waiting on a fence."
	 */
	*release_fence = -1;
	return buffer->flush();
}

int32_t cros_gralloc_driver::get_backing_store(buffer_handle_t handle, uint64_t *out_store)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		drv_log("Invalid handle.\n");
		return -EINVAL;
	}

	auto buffer_it = buffers_.find(hnd->id);
	if (buffer_it == buffers_.end()) {
		drv_log("Invalid Reference.\n");
		return -EINVAL;
	}

	auto buffer = buffer_it->second;
	*out_store = static_cast<uint64_t>(buffer->get_id());
	return 0;
}

int32_t cros_gralloc_driver::resource_info(buffer_handle_t handle, uint32_t strides[DRV_MAX_PLANES],
					   uint32_t offsets[DRV_MAX_PLANES],
					   uint64_t *format_modifier)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		drv_log("Invalid handle.\n");
		return -EINVAL;
	}

	auto buffer_it = buffers_.find(hnd->id);
	if (buffer_it == buffers_.end()) {
		drv_log("Invalid Reference.\n");
		return -EINVAL;
	}

	auto buffer = buffer_it->second;
	return buffer->resource_info(strides, offsets, format_modifier);
}

int32_t cros_gralloc_driver::get_reserved_region(buffer_handle_t handle,
						 void **reserved_region_addr,
						 uint64_t *reserved_region_size)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		drv_log("Invalid handle.\n");
		return -EINVAL;
	}

	auto buffer_it = buffers_.find(hnd->id);
	if (buffer_it == buffers_.end()) {
		drv_log("Invalid Reference.\n");
		return -EINVAL;
	}

	auto buffer = buffer_it->second;
	return buffer->get_reserved_region(reserved_region_addr, reserved_region_size);
}

uint32_t cros_gralloc_driver::get_resolved_drm_format(uint32_t drm_format, uint64_t use_flags)
{
	uint32_t resolved_format;
	uint64_t resolved_use_flags;

	drv_resolve_format_and_use_flags(drv_, drm_format, use_flags, &resolved_format,
					 &resolved_use_flags);

	return resolved_format;
}

void cros_gralloc_driver::with_buffer(cros_gralloc_handle_t hnd,
				      const std::function<void(cros_gralloc_buffer *)> &function)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto buffer_it = buffers_.find(hnd->id);
	if (buffer_it == buffers_.end()) {
		return;
	}

	auto buffer = buffer_it->second;
	function(buffer);
}

void cros_gralloc_driver::with_each_buffer(
    const std::function<void(cros_gralloc_buffer *)> &function)
{
	std::lock_guard<std::mutex> lock(mutex_);

	for (const auto &pair : buffers_)
		function(pair.second);
}
