/*
 * Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "cros_gralloc_helpers.h"
#include <hardware/gralloc.h>
#include "i915_private_android_types.h"

#include <sync/sync.h>

#ifdef USE_GRALLOC1
#include "i915_private_android.h"
const char* drmFormat2Str(int drm_format)
{
    static char buf[5];
    char *pDrmFormat = (char*) &drm_format;
    snprintf(buf, sizeof(buf), "%c%c%c%c", *pDrmFormat, *(pDrmFormat + 1),
             *(pDrmFormat + 2), *(pDrmFormat + 3));
    return buf;
}

bool is_flex_format(uint32_t format)
{
        switch (format) {
        case DRM_FORMAT_FLEX_IMPLEMENTATION_DEFINED:
        case DRM_FORMAT_FLEX_YCbCr_420_888:
                return true;
        default:
                return false;
        }
        return false;
}
#endif

bool flex_format_match(uint32_t descriptor_format, uint32_t handle_format, uint64_t usage)
{
	bool flag = usage & (GRALLOC_USAGE_HW_CAMERA_READ | GRALLOC_USAGE_HW_CAMERA_WRITE);

	/* HACK: See b/28671744 */
	if (HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED == descriptor_format &&
		((HAL_PIXEL_FORMAT_NV12 == handle_format && flag) ||
		(HAL_PIXEL_FORMAT_RGBX_8888 == handle_format && !flag)))
		return true;
	else if (HAL_PIXEL_FORMAT_YCBCR_420_888 == descriptor_format && HAL_PIXEL_FORMAT_NV12 == handle_format)
		return true;
	else
		return false;
}

uint32_t cros_gralloc_convert_format(int format)
{
	/*
	 * Conversion from HAL to fourcc-based DRV formats based on
	 * platform_android.c in mesa.
	 */

	switch (format) {
	case HAL_PIXEL_FORMAT_BGRA_8888:
		return DRM_FORMAT_ARGB8888;
	case HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED:
		return DRM_FORMAT_FLEX_IMPLEMENTATION_DEFINED;
	case HAL_PIXEL_FORMAT_RAW16:
		return DRM_FORMAT_R16;
	case HAL_PIXEL_FORMAT_RGB_565:
		return DRM_FORMAT_RGB565;
	case HAL_PIXEL_FORMAT_RGB_888:
		return DRM_FORMAT_BGR888;
	case HAL_PIXEL_FORMAT_RGBA_8888:
		return DRM_FORMAT_ABGR8888;
	case HAL_PIXEL_FORMAT_RGBX_8888:
		return DRM_FORMAT_XBGR8888;
	case HAL_PIXEL_FORMAT_YCbCr_420_888:
		return DRM_FORMAT_FLEX_YCbCr_420_888;
	case HAL_PIXEL_FORMAT_YV12:
		return DRM_FORMAT_YVU420_ANDROID;
	/*
	 * Choose DRM_FORMAT_R8 because <system/graphics.h> requires the buffers
	 * with a format HAL_PIXEL_FORMAT_BLOB have a height of 1, and width
	 * equal to their size in bytes.
	 */
	case HAL_PIXEL_FORMAT_BLOB:
		return DRM_FORMAT_R8;
#if ANDROID_VERSION >= 0x0a00
	case HAL_PIXEL_FORMAT_RGBA_1010102:
		return DRM_FORMAT_ABGR2101010;
	case HAL_PIXEL_FORMAT_RGBA_FP16:
		return DRM_FORMAT_ABGR16161616F;
#endif
	}

#ifdef USE_GRALLOC1
return i915_private_convert_format(format);
#else
	return DRM_FORMAT_NONE;
#endif
}

cros_gralloc_handle_t cros_gralloc_convert_handle(buffer_handle_t handle)
{
	auto hnd = reinterpret_cast<cros_gralloc_handle_t>(handle);
	if (!hnd || hnd->magic != cros_gralloc_magic)
		return nullptr;

	return hnd;
}

int32_t cros_gralloc_sync_wait(int32_t fence, bool close_fence)
{
	if (fence < 0)
		return 0;

	/*
	 * Wait initially for 1000 ms, and then wait indefinitely. The SYNC_IOC_WAIT
	 * documentation states the caller waits indefinitely on the fence if timeout < 0.
	 */
	int err = sync_wait(fence, 1000);
	if (err < 0) {
		drv_log("Timed out on sync wait, err = %s\n", strerror(errno));
		err = sync_wait(fence, -1);
		if (err < 0) {
			drv_log("sync wait error = %s\n", strerror(errno));
			return -errno;
		}
	}

	if (close_fence) {
		err = close(fence);
		if (err) {
			drv_log("Unable to close fence fd, err = %s\n", strerror(errno));
			return -errno;
		}
	}

	return 0;
}

#ifdef USE_GRALLOC1
int32_t cros_gralloc_sync_wait(int32_t acquire_fence)
{
        if (acquire_fence < 0)
                return 0;

        /*
         * Wait initially for 1000 ms, and then wait indefinitely. The SYNC_IOC_WAIT
         * documentation states the caller waits indefinitely on the fence if timeout < 0.
         */
        int err = sync_wait(acquire_fence, 1000);
        if (err < 0) {
                drv_log("Timed out on sync wait, err = %s", strerror(errno));
                err = sync_wait(acquire_fence, -1);
                if (err < 0) {
                        drv_log("sync wait error = %s", strerror(errno));
                        return -errno;
                }
        }

        err = close(acquire_fence);
        if (err) {
                drv_log("Unable to close fence fd, err = %s", strerror(errno));
                return -errno;
        }

        return 0;
}
#endif
