/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifdef DRV_MESON

#include "drv_priv.h"
#include "helpers.h"
#include "util.h"

static const uint32_t scanout_render_formats[] = { DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888,
						   DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888,
						   DRM_FORMAT_BGR888,	DRM_FORMAT_BGR565 };

static const uint32_t texture_only_formats[] = { DRM_FORMAT_NV12, DRM_FORMAT_NV21,
						 DRM_FORMAT_YVU420, DRM_FORMAT_YVU420_ANDROID };

static int meson_init(struct driver *drv)
{
	drv_add_combinations(drv, scanout_render_formats, ARRAY_SIZE(scanout_render_formats),
			     &LINEAR_METADATA, BO_USE_RENDER_MASK | BO_USE_SCANOUT);

	drv_add_combinations(drv, texture_only_formats, ARRAY_SIZE(texture_only_formats),
			     &LINEAR_METADATA, BO_USE_TEXTURE_MASK);

	drv_modify_combination(drv, DRM_FORMAT_NV12, &LINEAR_METADATA,
			       BO_USE_HW_VIDEO_ENCODER | BO_USE_HW_VIDEO_DECODER |
				   BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE);
	drv_modify_combination(drv, DRM_FORMAT_NV21, &LINEAR_METADATA, BO_USE_HW_VIDEO_ENCODER);

	return drv_modify_linear_combinations(drv);
}

const struct backend backend_meson = {
	.name = "meson",
	.init = meson_init,
	.bo_create = drv_dumb_bo_create,
	.bo_destroy = drv_dumb_bo_destroy,
	.bo_import = drv_prime_bo_import,
	.bo_map = drv_dumb_bo_map,
	.bo_unmap = drv_bo_munmap,
};

#endif
