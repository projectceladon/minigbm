/*
 * Copyright 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifdef DRV_XE

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>

#include "drv_helpers.h"
#include "drv_priv.h"
#include "external/xe_drm.h"
#include "util.h"

#define XE_CACHELINE_SIZE 64
#define XE_CACHELINE_MASK (XE_CACHELINE_SIZE - 1)

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

static const uint32_t source_formats[] = { DRM_FORMAT_P010, DRM_FORMAT_NV12_INTEL };

#if !defined(DRM_CAP_CURSOR_WIDTH)
#define DRM_CAP_CURSOR_WIDTH 0x8
#endif

#if !defined(DRM_CAP_CURSOR_HEIGHT)
#define DRM_CAP_CURSOR_HEIGHT 0x9
#endif

static const uint32_t kDefaultCursorWidth = 64;
static const uint32_t kDefaultCursorHeight = 64;

#define BO_USE_CAMERA_MASK BO_USE_CAMERA_READ | BO_USE_SCANOUT | BO_USE_CAMERA_WRITE


static const uint64_t gen12_modifier_order[] = { I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS,
						 I915_FORMAT_MOD_Y_TILED, I915_FORMAT_MOD_X_TILED,
						 DRM_FORMAT_MOD_LINEAR };

static const uint64_t xe_lpdp_modifier_order[] = { I915_FORMAT_MOD_4_TILED, I915_FORMAT_MOD_X_TILED,
						   DRM_FORMAT_MOD_LINEAR };

struct modifier_support_t {
	const uint64_t *order;
	uint32_t count;
};

struct xe_device {
	uint32_t graphics_version;
	int32_t has_hw_protection;
	struct modifier_support_t modifier;
	int device_id;
	bool is_xelpd;
	/*TODO : cleanup is_mtl_or_newer to avoid adding variables for every new platforms */
	bool is_mtl_or_newer;
	bool has_mmap_offset;
	bool has_local_mem;
	uint64_t cursor_width;
	uint64_t cursor_height;
};

static void xe_info_from_device_id(struct xe_device *xe)
{
	const uint16_t gen12_ids[] = {
		0x4c8a, 0x4c8b, 0x4c8c, 0x4c90, 0x4c9a, 0x4680, 0x4681, 0x4682, 0x4683, 0x4688,
		0x4689, 0x4690, 0x4691, 0x4692, 0x4693, 0x4698, 0x4699, 0x4626, 0x4628, 0x462a,
		0x46a0, 0x46a1, 0x46a2, 0x46a3, 0x46a6, 0x46a8, 0x46aa, 0x46b0, 0x46b1, 0x46b2,
		0x46b3, 0x46c0, 0x46c1, 0x46c2, 0x46c3, 0x9A40, 0x9A49, 0x9A59, 0x9A60, 0x9A68,
		0x9A70, 0x9A78, 0x9AC0, 0x9AC9, 0x9AD9, 0x9AF8, 0x4905, 0x4906, 0x4907, 0x4908
	};
	const uint16_t adlp_ids[] = { 0x46A0, 0x46A1, 0x46A2, 0x46A3, 0x46A6, 0x46A8, 0x46AA,
				      0x462A, 0x4626, 0x4628, 0x46B0, 0x46B1, 0x46B2, 0x46B3,
				      0x46C0, 0x46C1, 0x46C2, 0x46C3, 0x46D0, 0x46D1, 0x46D2 };

	const uint16_t rplp_ids[] = { 0xA720, 0xA721, 0xA7A0, 0xA7A1, 0xA7A8, 0xA7A9 };

	const uint16_t mtl_ids[] = { 0x7D40, 0x7D60, 0x7D45, 0x7D55, 0x7DD5 };

	const uint16_t lnl_ids[] = { 0x6420, 0x64A0, 0x64B0};

	const uint16_t ptl_ids[] = { 0xB080, 0xB090, 0xB0A0, 0xB0B0, 0xB0FF};

	unsigned i;
	xe->graphics_version = 0;
	xe->is_xelpd = false;
	xe->is_mtl_or_newer = false;

	/* Gen 12 */
	for (i = 0; i < ARRAY_SIZE(gen12_ids); i++)
		if (gen12_ids[i] == xe->device_id)
			xe->graphics_version = 12;

	for (i = 0; i < ARRAY_SIZE(adlp_ids); i++)
		if (adlp_ids[i] == xe->device_id) {
			xe->is_xelpd = true;
			xe->graphics_version = 12;
		}

	for (i = 0; i < ARRAY_SIZE(rplp_ids); i++)
		if (rplp_ids[i] == xe->device_id) {
			xe->is_xelpd = true;
			xe->graphics_version = 12;
		}

	for (i = 0; i < ARRAY_SIZE(mtl_ids); i++)
		if (mtl_ids[i] == xe->device_id) {
			xe->graphics_version = 12;
			xe->is_mtl_or_newer = true;
		}

	for (i = 0; i < ARRAY_SIZE(ptl_ids); i++)
		if (ptl_ids[i] == xe->device_id) {
			xe->graphics_version = 12;
			xe->is_mtl_or_newer = true;
		}

	for (i = 0; i < ARRAY_SIZE(lnl_ids); i++)
		if (lnl_ids[i] == xe->device_id) {
			xe->graphics_version = 12;
			xe->is_mtl_or_newer = true;
		}
}

static void xe_get_modifier_order(struct xe_device *xe)
{
	if (xe->is_mtl_or_newer) {
		xe->modifier.order = xe_lpdp_modifier_order;
		xe->modifier.count = ARRAY_SIZE(xe_lpdp_modifier_order);
	} else {
		xe->modifier.order = gen12_modifier_order;
		xe->modifier.count = ARRAY_SIZE(gen12_modifier_order);
	}
}

static uint64_t unset_flags(uint64_t current_flags, uint64_t mask)
{
	uint64_t value = current_flags & ~mask;
	return value;
}

static int xe_add_combinations(struct driver *drv)
{
	struct xe_device *xe = drv->priv;

	const uint64_t scanout_and_render = BO_USE_RENDER_MASK | BO_USE_SCANOUT;
	const uint64_t render = BO_USE_RENDER_MASK;
	const uint64_t texture_only = BO_USE_TEXTURE_MASK;
	uint64_t render_flags = BO_USE_RENDER_MASK;
	uint64_t texture_flags = BO_USE_TEXTURE_MASK;

	// HW protected buffers also need to be scanned out.
	const uint64_t hw_protected =
	    xe->has_hw_protection ? (BO_USE_PROTECTED | BO_USE_SCANOUT) : 0;

	const uint64_t linear_mask = BO_USE_RENDERSCRIPT | BO_USE_LINEAR | BO_USE_SW_READ_OFTEN |
				     BO_USE_SW_WRITE_OFTEN | BO_USE_SW_READ_RARELY |
				     BO_USE_SW_WRITE_RARELY;

	struct format_metadata metadata_linear = { .tiling = XE_TILING_NONE,
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


	const uint64_t render_not_linear = unset_flags(render, linear_mask);
	const uint64_t scanout_and_render_not_linear = render_not_linear | BO_USE_SCANOUT;
	uint64_t texture_flags_video =
           unset_flags(texture_flags, BO_USE_RENDERSCRIPT | BO_USE_SW_WRITE_OFTEN |
                                          BO_USE_SW_READ_OFTEN | BO_USE_LINEAR);


	struct format_metadata metadata_x_tiled = { .tiling = XE_TILING_X,
						    .priority = 2,
						    .modifier = I915_FORMAT_MOD_X_TILED };

	drv_add_combinations(drv, render_formats, ARRAY_SIZE(render_formats), &metadata_x_tiled,
			     render_not_linear);
	drv_add_combinations(drv, scanout_render_formats, ARRAY_SIZE(scanout_render_formats),
			     &metadata_x_tiled, scanout_and_render_not_linear);
	drv_add_combinations(drv, linear_source_formats, ARRAY_SIZE(linear_source_formats),
                             &metadata_x_tiled, texture_flags_video | BO_USE_CAMERA_MASK);


	if (xe->is_mtl_or_newer) {
		struct format_metadata metadata_4_tiled = { .tiling = XE_TILING_4,
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
		drv_add_combinations(drv, render_formats, ARRAY_SIZE(render_formats),
				     &metadata_4_tiled, render_not_linear);
		drv_add_combinations(drv, scanout_render_formats,
				     ARRAY_SIZE(scanout_render_formats), &metadata_4_tiled,
				     render_not_linear);
                drv_add_combinations(drv, source_formats, ARRAY_SIZE(source_formats), &metadata_4_tiled,
                                     texture_flags | BO_USE_NON_GPU_HW);

	} else {
		struct format_metadata metadata_y_tiled = { .tiling = XE_TILING_Y,
							    .priority = 3,
							    .modifier = I915_FORMAT_MOD_Y_TILED };
/* Support y-tiled NV12 and P010 for libva */
#ifdef I915_SCANOUT_Y_TILED
		const uint64_t nv12_usage =
		    BO_USE_TEXTURE | BO_USE_HW_VIDEO_DECODER | BO_USE_SCANOUT | hw_protected;
		const uint64_t p010_usage = BO_USE_TEXTURE | BO_USE_HW_VIDEO_DECODER |
					    hw_protected | BO_USE_SCANOUT;
#else
		const uint64_t nv12_usage = BO_USE_TEXTURE | BO_USE_HW_VIDEO_DECODER;
		const uint64_t p010_usage = nv12_usage;
#endif
		drv_add_combination(drv, DRM_FORMAT_NV12, &metadata_y_tiled, nv12_usage);
		drv_add_combination(drv, DRM_FORMAT_P010, &metadata_y_tiled, p010_usage);
		drv_add_combinations(drv, render_formats, ARRAY_SIZE(render_formats),
				     &metadata_y_tiled, render_not_linear);
		/* Y-tiled scanout isn't available on old platforms so we add
		 * |scanout_render_formats| without that USE flag.
		 */
		drv_add_combinations(drv, scanout_render_formats,
				     ARRAY_SIZE(scanout_render_formats), &metadata_y_tiled,
				     render_not_linear);
		drv_add_combinations(drv, source_formats, ARRAY_SIZE(source_formats), &metadata_y_tiled,
				     texture_flags | BO_USE_NON_GPU_HW);

	}
	return 0;
}

static int xe_align_dimensions(struct bo *bo, uint32_t format, uint32_t tiling, uint32_t *stride,
				 uint32_t *aligned_height)
{
	struct xe_device *xe = bo->drv->priv;
	uint32_t horizontal_alignment;
	uint32_t vertical_alignment;

	switch (tiling) {
	default:
	case XE_TILING_NONE:
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

	case XE_TILING_X:
		horizontal_alignment = 512;
		vertical_alignment = 8;
		break;

	case XE_TILING_Y:
	case XE_TILING_4:
		horizontal_alignment = 128;
		vertical_alignment = 32;
		break;
	}

	*aligned_height = ALIGN(*aligned_height, vertical_alignment);
	if (DRM_FORMAT_R8 != bo->meta.format)
		*stride = ALIGN(*stride, horizontal_alignment);

	return 0;
}

static void xe_clflush(void *start, size_t size)
{
	void *p = (void *)(((uintptr_t)start) & ~XE_CACHELINE_MASK);
	void *end = (void *)((uintptr_t)start + size);

	__builtin_ia32_mfence();
	while (p < end) {
		__builtin_ia32_clflush(p);
		p = (void *)((uintptr_t)p + XE_CACHELINE_SIZE);
	}
}

static bool xe_query_device_info(struct driver *drv, struct xe_device *xe)
{
	struct drm_xe_device_query query = {
		.extensions = 0,
		.query = DRM_XE_DEVICE_QUERY_CONFIG,
		.size = 0,
		.data = 0,
	};

	if (drmIoctl(drv->fd, DRM_IOCTL_XE_DEVICE_QUERY, &query))
		return false;

	struct drm_xe_query_config *config = calloc(1, query.size);
	if (!config)
		return false;

	query.data = (uintptr_t)config;
	if (drmIoctl(drv->fd, DRM_IOCTL_XE_DEVICE_QUERY, &query)){
		free(config);
		return false;
	}

	xe->device_id =
		((config->info[DRM_XE_QUERY_CONFIG_REV_AND_DEVICE_ID] << 16)>>16) & 0xFFFF;

	if (config->info[DRM_XE_QUERY_CONFIG_FLAGS] & DRM_XE_QUERY_CONFIG_FLAG_HAS_VRAM)
		xe->has_local_mem = true;
	else
		xe->has_local_mem = false;

	free(config);
	return true;
}

static int xe_init(struct driver *drv)
{
	int ret, val;
	struct xe_device *xe;

	xe = calloc(1, sizeof(*xe));
	if (!xe)
		return -ENOMEM;

	if (!xe_query_device_info(drv, xe)) {
		drv_loge("Failed to query device id using DRM_IOCTL_XE_DEVICE_QUERY");
		if (!xe)
			free(xe);
		return -EINVAL;
	}

	/* must call before xe->graphics_version is used anywhere else */
	xe_info_from_device_id(xe);

	xe_get_modifier_order(xe);

	/* TODO: Xe still don't have support for protected content */
	xe->has_hw_protection = 0;

	uint64_t width = 0, height = 0;
	if (drmGetCap(drv->fd, DRM_CAP_CURSOR_WIDTH, &width)) {
		drv_loge("cannot get cursor width. \n");
	} else if (drmGetCap(drv->fd, DRM_CAP_CURSOR_HEIGHT, &height)) {
		drv_loge("cannot get cursor height. \n");
	}

	if (!width)
		width = kDefaultCursorWidth;
	xe->cursor_width = width;

	if (!height)
		height = kDefaultCursorHeight;
	xe->cursor_height = height;

	drv->priv = xe;
	return xe_add_combinations(drv);
}

/*
 * Returns true if the height of a buffer of the given format should be aligned
 * to the largest coded unit (LCU) assuming that it will be used for video. This
 * is based on gmmlib's GmmIsYUVFormatLCUAligned().
 */
static bool xe_format_needs_LCU_alignment(uint32_t format, size_t plane,
					    const struct xe_device *xe)
{
	switch (format) {
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_P010:
	case DRM_FORMAT_P016:
		return (xe->graphics_version == 12) && plane == 1;
	}
	return false;
}

static int xe_bo_from_format(struct bo *bo, uint32_t width, uint32_t height, uint32_t format)
{
	uint32_t offset;
	size_t plane;
	int ret, pagesize;
	struct xe_device *xe = bo->drv->priv;

	offset = 0;
	pagesize = getpagesize();

	for (plane = 0; plane < drv_num_planes_from_format(format); plane++) {
		uint32_t stride = drv_stride_from_format(format, width, plane);
		uint32_t plane_height = drv_height_from_format(format, height, plane);

		if (bo->meta.tiling != XE_TILING_NONE)
			assert(IS_ALIGNED(offset, pagesize));

		ret = xe_align_dimensions(bo, format, bo->meta.tiling, &stride, &plane_height);
		if (ret)
			return ret;

		if (xe_format_needs_LCU_alignment(format, plane, xe)) {
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

static size_t xe_num_planes_from_modifier(struct driver *drv, uint32_t format, uint64_t modifier)
{
	size_t num_planes = drv_num_planes_from_format(format);
	if (modifier == I915_FORMAT_MOD_Y_TILED_CCS ||
	    modifier == I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS) {
		assert(num_planes == 1);
		return 2;
	}

	return num_planes;
}

static int xe_bo_compute_metadata(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
				    uint64_t use_flags, const uint64_t *modifiers, uint32_t count)
{
	uint64_t modifier;
	struct xe_device *xe = bo->drv->priv;

	if (modifiers) {
		modifier =
		    drv_pick_modifier(modifiers, count, xe->modifier.order, xe->modifier.count);
	} else {
		struct combination *combo = drv_get_combination(bo->drv, format, use_flags);
		if (!combo)
			return -EINVAL;
		modifier = combo->metadata.modifier;
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

	switch (modifier) {
	case DRM_FORMAT_MOD_LINEAR:
		bo->meta.tiling = XE_TILING_NONE;
		break;
	case I915_FORMAT_MOD_X_TILED:
		bo->meta.tiling = XE_TILING_X;
		break;
	case I915_FORMAT_MOD_Y_TILED:
	case I915_FORMAT_MOD_Y_TILED_CCS:
       case I915_FORMAT_MOD_Yf_TILED:
       case I915_FORMAT_MOD_Yf_TILED_CCS:

	/* For now support only XE_TILING_Y as this works with all
	 * IPs(render/media/display)
	 */
	case I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS:
		bo->meta.tiling = XE_TILING_Y;
		break;
	case I915_FORMAT_MOD_4_TILED:
		bo->meta.tiling = XE_TILING_4;
		break;
	}

	bo->meta.format_modifier = modifier;

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

		bo->meta.num_planes = xe_num_planes_from_modifier(bo->drv, format, modifier);
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

		if (xe->is_xelpd && (stride > 1)) {
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
		bo->meta.num_planes = xe_num_planes_from_modifier(bo->drv, format, modifier);
		bo->meta.total_size = bo->meta.sizes[0] + bo->meta.sizes[1];
	} else {
		return xe_bo_from_format(bo, width, height, format);
	}
	return 0;
}

static int xe_bo_create_from_metadata(struct bo *bo)
{
	int ret;
	size_t plane;
	uint32_t gem_handle;
	uint32_t vm = 0;
	struct xe_device *xe = bo->drv->priv;

	/* From xe_drm.h: If a VM is specified, this BO must:
	 * 1. Only ever be bound to that VM.
	 * 2. Cannot be exported as a PRIME fd.
	 *.vm_id = alloc_flags & ANV_BO_ALLOC_EXTERNAL ? 0 : device->vm_id,
	 * Should all buffers be defined as external? See here:
	 * https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/intel/vulkan/xe/anv_kmd_backend.c?ref_type=heads#L60
	 */
	vm = 0;

	struct drm_xe_gem_create gem_create = {
		.vm_id = vm,
		.size = bo->meta.total_size,
		.flags = DRM_XE_GEM_CREATE_FLAG_SCANOUT,
	};

	/* FIXME: let's assume iGPU with SYSMEM is only supported */
	gem_create.placement |= BITFIELD_BIT(DRM_XE_MEM_REGION_CLASS_SYSMEM);
	gem_create.cpu_caching = DRM_XE_GEM_CPU_CACHING_WC;

	ret = drmIoctl(bo->drv->fd, DRM_IOCTL_XE_GEM_CREATE, &gem_create);
	if (ret) {
		drv_loge("DRM_IOCTL_I915_GEM_CREATE failed (size=%llu)\n", gem_create.size);
		return -errno;
	}

	gem_handle = gem_create.handle;

	for (plane = 0; plane < bo->meta.num_planes; plane++)
		bo->handles[plane].u32 = gem_handle;

	return 0;
}

static void xe_close(struct driver *drv)
{
	free(drv->priv);
	drv->priv = NULL;
}

static int xe_bo_import(struct bo *bo, struct drv_import_fd_data *data)
{
	int ret;
	struct xe_device *xe = bo->drv->priv;

	bo->meta.num_planes =
	    xe_num_planes_from_modifier(bo->drv, data->format, data->format_modifier);

	ret = drv_prime_bo_import(bo, data);
	if (ret)
		return ret;

	return 0;
}

static void *xe_bo_map(struct bo *bo, struct vma *vma, uint32_t map_flags)
{
	int ret;
	void *addr = MAP_FAILED;
	struct xe_device *xe = bo->drv->priv;

	if ((bo->meta.format_modifier == I915_FORMAT_MOD_Y_TILED_CCS) ||
	    (bo->meta.format_modifier == I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS) ||
	    (bo->meta.format_modifier == I915_FORMAT_MOD_4_TILED))
		return MAP_FAILED;

	if (bo->meta.tiling == XE_TILING_NONE) {
		struct drm_xe_gem_mmap_offset gem_map = { 0 };
		gem_map.handle = bo->handles[0].u32;

		/* Get the fake offset back */
		ret = drmIoctl(bo->drv->fd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &gem_map);
		if (ret == 0)
			addr = mmap(0, bo->meta.total_size, drv_get_prot(map_flags),
				    MAP_SHARED, bo->drv->fd, gem_map.offset);
	}

	if (addr == MAP_FAILED) {
		drv_loge("xe GEM mmap failed\n");
		return addr;
	}

	vma->length = bo->meta.total_size;
	return addr;
}

static int xe_bo_invalidate(struct bo *bo, struct mapping *mapping)
{
	// FIXME: Add changes once set domain support is added in xe.
	return 0;
}

static int xe_bo_flush(struct bo *bo, struct mapping *mapping)
{
	struct xe_device *xe = bo->drv->priv;
	if (bo->meta.tiling == XE_TILING_NONE)
		xe_clflush(mapping->vma->addr, mapping->vma->length);

	return 0;
}

const struct backend backend_xe = {
	.name = "xe",
	.init = xe_init,
	.close = xe_close,
	.bo_compute_metadata = xe_bo_compute_metadata,
	.bo_create_from_metadata = xe_bo_create_from_metadata,
	.bo_destroy = drv_gem_bo_destroy,
	.bo_import = xe_bo_import,
	.bo_map = xe_bo_map,
	.bo_unmap = drv_bo_munmap,
	.bo_invalidate = xe_bo_invalidate,
	.bo_flush = xe_bo_flush,
	.resolve_format_and_use_flags = drv_resolve_format_and_use_flags_helper,
	.num_planes_from_modifier = xe_num_planes_from_modifier,
};

#endif
