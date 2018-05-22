/*
 * Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <system/graphics.h>
#include <cutils/log.h>
#include <cutils/properties.h>


#include "drv_priv.h"
#include "helpers.h"
#include "util.h"
#include "i915_private.h"

#ifdef DRV_AMDGPU
extern struct backend backend_amdgpu;
#endif
extern struct backend backend_cirrus;
extern struct backend backend_evdi;
#ifdef DRV_EXYNOS
extern struct backend backend_exynos;
#endif
extern struct backend backend_gma500;
#ifdef DRV_I915
extern struct backend backend_i915;
extern struct backend backend_i915_bpo;

#endif
#ifdef DRV_MARVELL
extern struct backend backend_marvell;
#endif
#ifdef DRV_MEDIATEK
extern struct backend backend_mediatek;
#endif
extern struct backend backend_nouveau;
#ifdef DRV_RADEON
extern struct backend backend_radeon;
#endif
#ifdef DRV_ROCKCHIP
extern struct backend backend_rockchip;
#endif
#ifdef DRV_TEGRA
extern struct backend backend_tegra;
#endif
extern struct backend backend_udl;
#ifdef DRV_VC4
extern struct backend backend_vc4;
#endif
extern struct backend backend_vgem;
extern struct backend backend_virtio_gpu;

static struct backend *drv_get_backend(int fd)
{
	drmVersionPtr drm_version;
	unsigned int i;

	drm_version = drmGetVersion(fd);

	if (!drm_version)
		return NULL;

	struct backend *backend_list[] = {
#ifdef DRV_AMDGPU
		&backend_amdgpu,
#endif
		&backend_cirrus,   &backend_evdi,
#ifdef DRV_EXYNOS
		&backend_exynos,
#endif
		&backend_gma500,
#ifdef DRV_I915
		&backend_i915,
	    &backend_i915_bpo,
#endif
#ifdef DRV_MARVELL
		&backend_marvell,
#endif
#ifdef DRV_MEDIATEK
		&backend_mediatek,
#endif
		&backend_nouveau,
#ifdef DRV_RADEON
		&backend_radeon,
#endif
#ifdef DRV_ROCKCHIP
		&backend_rockchip,
#endif
#ifdef DRV_TEGRA
		&backend_tegra,
#endif
		&backend_udl,
#ifdef DRV_VC4
		&backend_vc4,
#endif
		&backend_vgem,     &backend_virtio_gpu,
	};

	for (i = 0; i < ARRAY_SIZE(backend_list); i++)
		if (!strcmp(drm_version->name, backend_list[i]->name)) {
			drmFreeVersion(drm_version);
			return backend_list[i];
		}

	drmFreeVersion(drm_version);
	return NULL;
}

struct driver *drv_create(int fd)
{
	struct driver *drv;
	int ret;
    
	drv = (struct driver *)calloc(1, sizeof(*drv));

	if (!drv)
		return NULL;

	drv->fd = fd;
	drv->backend = drv_get_backend(fd);

	if (!drv->backend) {
        ALOGE("%s: %d : get backend failed",__func__, __LINE__);
		goto free_driver;
    }

	drv->buffer_table = drmHashCreate();
	if (!drv->buffer_table)
		goto free_driver;

	drv->map_table = drmHashCreate();
	if (!drv->map_table)
		goto free_buffer_table;

	/* Start with a power of 2 number of allocations. */
	drv->combos.allocations = 2;
	drv->combos.size = 0;

	drv->combos.data = calloc(drv->combos.allocations, sizeof(struct combination));
	if (!drv->combos.data)
		goto free_map_table;

	if (drv->backend->init) {
		ret = drv->backend->init(drv);
		if (ret) {
            ALOGE("%s: %d : backend init failed",__func__, __LINE__);
			free(drv->combos.data);
			goto free_map_table;
		}
	}


	ATOMIC_VAR_INIT(drv->driver_lock);

	return drv;

free_map_table:
	drmHashDestroy(drv->map_table);
free_buffer_table:
	drmHashDestroy(drv->buffer_table);
free_driver:
	free(drv);
	return NULL;
}

void drv_destroy(struct driver *drv)
{
	ATOMIC_LOCK(&drv->driver_lock);

       close(drv->fd);

	if (drv->backend->close)
		drv->backend->close(drv);

	drmHashDestroy(drv->buffer_table);
	drmHashDestroy(drv->map_table);

	free(drv->combos.data);

	ATOMIC_UNLOCK(&drv->driver_lock);

	free(drv);
}

int drv_get_fd(struct driver *drv)
{
	return drv->fd;
}

const char *drv_get_name(struct driver *drv)
{
	return drv->backend->name;
}

struct combination *drv_get_combination(struct driver *drv, uint32_t format, uint64_t use_flags)
{
	struct combination *curr, *best;

	if (format == DRM_FORMAT_NONE || use_flags == BO_USE_NONE)
		return 0;

	best = NULL;
	uint32_t i;
	for (i = 0; i < drv->combos.size; i++) {
		curr = &drv->combos.data[i];
		if ((format == curr->format) && use_flags == (curr->use_flags & use_flags))
			if (!best || best->metadata.priority < curr->metadata.priority)
				best = curr;
	}

	return best;
}

struct bo *drv_bo_new(struct driver *drv, uint32_t width, uint32_t height, uint32_t format,
		      uint64_t use_flags)
{

	struct bo *bo;
	bo = (struct bo *)calloc(1, sizeof(*bo));

	if (!bo)
		return NULL;

	bo->drv = drv;
	bo->width = width;
	bo->height = height;
	bo->format = format;
	bo->use_flags = use_flags;
	bo->num_planes = drv_num_planes_from_format(format);

	if (!bo->num_planes) {
		free(bo);
		return NULL;
	}

	return bo;
}

struct bo *drv_bo_create(struct driver *drv, uint32_t width, uint32_t height, uint32_t format,
			 uint64_t use_flags)
{
	int ret;
	size_t plane;
	struct bo *bo;

	bo = drv_bo_new(drv, width, height, format, use_flags);

	if (!bo)
		return NULL;

	ret = drv->backend->bo_create(bo, width, height, format, use_flags);

	if (ret) {
		free(bo);
		return NULL;
	}

	ATOMIC_LOCK(&drv->driver_lock);

	for (plane = 0; plane < bo->num_planes; plane++) {
		if (plane > 0)
			assert(bo->offsets[plane] >= bo->offsets[plane - 1]);

		drv_increment_reference_count(drv, bo, plane);
	}

	ATOMIC_UNLOCK(&drv->driver_lock);

	return bo;
}

struct bo *drv_bo_create_with_modifiers(struct driver *drv, uint32_t width, uint32_t height,
					uint32_t format, const uint64_t *modifiers, uint32_t count)
{
	int ret;
	size_t plane;
	struct bo *bo;

	if (!drv->backend->bo_create_with_modifiers) {
		errno = ENOENT;
		return NULL;
	}

	bo = drv_bo_new(drv, width, height, format, BO_USE_NONE);

	if (!bo)
		return NULL;

	ret = drv->backend->bo_create_with_modifiers(bo, width, height, format, modifiers, count);

	if (ret) {
		free(bo);
		return NULL;
	}

	ATOMIC_LOCK(&drv->driver_lock);

	for (plane = 0; plane < bo->num_planes; plane++) {
		if (plane > 0)
			assert(bo->offsets[plane] >= bo->offsets[plane - 1]);

		drv_increment_reference_count(drv, bo, plane);
	}

	ATOMIC_UNLOCK(&drv->driver_lock);

	return bo;
}

void drv_bo_destroy(struct bo *bo)
{
	size_t plane;
	uintptr_t total = 0;
	struct driver *drv = bo->drv;

	ATOMIC_LOCK(&drv->driver_lock);

	for (plane = 0; plane < bo->num_planes; plane++)
		drv_decrement_reference_count(drv, bo, plane);

	for (plane = 0; plane < bo->num_planes; plane++)
		total += drv_get_reference_count(drv, bo, plane);

	ATOMIC_UNLOCK(&drv->driver_lock);

	if (total == 0) {
		assert(drv_map_info_destroy(bo) == 0);
		bo->drv->backend->bo_destroy(bo);
	}

	free(bo);
}

struct bo *drv_bo_import(struct driver *drv, struct drv_import_fd_data *data)
{
	int ret;
	size_t plane;
	struct bo *bo;
	off_t seek_end;

	bo = drv_bo_new(drv, data->width, data->height, data->format, data->use_flags);

	if (!bo)
		return NULL;

	ret = drv->backend->bo_import(bo, data);
	if (ret) {
		free(bo);
		return NULL;
	}

	for (plane = 0; plane < bo->num_planes; plane++) {
		bo->strides[plane] = data->strides[plane];
		bo->offsets[plane] = data->offsets[plane];
		bo->format_modifiers[plane] = data->format_modifiers[plane];

		seek_end = lseek(data->fds[plane], 0, SEEK_END);
		if (seek_end == (off_t)(-1)) {
			fprintf(stderr, "drv: lseek() failed with %s\n", strerror(errno));
			goto destroy_bo;
		}

		lseek(data->fds[plane], 0, SEEK_SET);
		if (plane == bo->num_planes - 1 || data->offsets[plane + 1] == 0)
			bo->sizes[plane] = seek_end - data->offsets[plane];
		else
			bo->sizes[plane] = data->offsets[plane + 1] - data->offsets[plane];

		if ((int64_t)bo->offsets[plane] + bo->sizes[plane] > seek_end) {
			fprintf(stderr, "drv: buffer size is too large.\n");
			goto destroy_bo;
		}

		bo->total_size += bo->sizes[plane];
	}

	return bo;

destroy_bo:
	drv_bo_destroy(bo);
	return NULL;
}

void *drv_bo_map(struct bo *bo, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
		 uint32_t map_flags, struct map_info **map_data, size_t plane)
{
	void *ptr;
	uint8_t *addr;
	size_t offset;
	struct map_info *data;

	assert(width > 0);
	assert(height > 0);
	assert(x + width <= drv_bo_get_width(bo));
	assert(y + height <= drv_bo_get_height(bo));
	assert(BO_MAP_READ_WRITE & map_flags);
	/* No CPU access for protected buffers. */
	assert(!(bo->use_flags & BO_USE_PROTECTED));

	ATOMIC_LOCK(&bo->drv->driver_lock);

	if (!drmHashLookup(bo->drv->map_table, bo->handles[plane].u32, &ptr)) {
		data = (struct map_info *)ptr;
		/* TODO(gsingh): support mapping same buffer with different flags. */
		assert(data->map_flags == map_flags);
		data->refcount++;
		goto success;
	}

	data = calloc(1, sizeof(*data));
	addr = bo->drv->backend->bo_map(bo, data, plane, map_flags);
	if (addr == MAP_FAILED) {
		*map_data = NULL;
		free(data);
		ATOMIC_UNLOCK(&bo->drv->driver_lock);
		return MAP_FAILED;
	}

	data->refcount = 1;
	data->addr = addr;
	data->handle = bo->handles[plane].u32;
	data->map_flags = map_flags;
	drmHashInsert(bo->drv->map_table, bo->handles[plane].u32, (void *)data);

success:
	drv_bo_invalidate(bo, data);
	*map_data = data;
	offset = drv_bo_get_plane_stride(bo, plane) * y;
	offset += drv_stride_from_format(bo->format, x, plane);
	addr = (uint8_t *)data->addr;
	addr += drv_bo_get_plane_offset(bo, plane) + offset;
	ATOMIC_UNLOCK(&bo->drv->driver_lock);

	return (void *)addr;
}

int drv_bo_unmap(struct bo *bo, struct map_info *data)
{
	int ret = drv_bo_flush(bo, data);
	if (ret)
		return ret;

	ATOMIC_LOCK(&bo->drv->driver_lock);

	if (!--data->refcount) {
		ret = bo->drv->backend->bo_unmap(bo, data);
		drmHashDelete(bo->drv->map_table, data->handle);
		free(data);
	}

	ATOMIC_UNLOCK(&bo->drv->driver_lock);;

	return ret;
}

int drv_bo_invalidate(struct bo *bo, struct map_info *data)
{
	int ret = 0;
	assert(data);
	assert(data->refcount >= 0);

	if (bo->drv->backend->bo_invalidate)
		ret = bo->drv->backend->bo_invalidate(bo, data);

	return ret;
}

int drv_bo_flush(struct bo *bo, struct map_info *data)
{
	int ret = 0;
	assert(data);
	assert(data->refcount >= 0);
	assert(!(bo->use_flags & BO_USE_PROTECTED));

	if (bo->drv->backend->bo_flush)
		ret = bo->drv->backend->bo_flush(bo, data);

	return ret;
}

uint32_t drv_bo_get_width(struct bo *bo)
{
	return bo->width;
}

uint32_t drv_bo_get_height(struct bo *bo)
{
	return bo->height;
}

uint32_t drv_bo_get_stride_or_tiling(struct bo *bo)
{
	return bo->tiling ? bo->tiling : drv_bo_get_plane_stride(bo, 0);
}

size_t drv_bo_get_num_planes(struct bo *bo)
{
	return bo->num_planes;
}

union bo_handle drv_bo_get_plane_handle(struct bo *bo, size_t plane)
{
	return bo->handles[plane];
}

#ifndef DRM_RDWR
#define DRM_RDWR O_RDWR
#endif

int drv_bo_get_plane_fd(struct bo *bo, size_t plane)
{

	int ret = 0;
    int fd = -1;
    uint32_t flags = 0;


    // Note  :  kernel 4.4 can't support DRM_RDWR flag
    // flags = DRM_CLOEXEC | DRM_RDWR;
    flags = DRM_CLOEXEC;

	assert(plane < bo->num_planes);

	ret = drmPrimeHandleToFD(bo->drv->fd, bo->handles[plane].u32, flags, &fd);
    if(ret) {
        ALOGE("%s : %d : drmPrimeHandleToFD failed(fd = %d, handle = %d, flags = %x, prime_fd = %d) ret = %d",
            __func__, __LINE__, bo->drv->fd, bo->handles[plane].u32, flags, fd, ret);
    }

	return (ret) ? ret : fd;
}

uint32_t drv_bo_get_plane_offset(struct bo *bo, size_t plane)
{
	assert(plane < bo->num_planes);
	return bo->offsets[plane];
}

uint32_t drv_bo_get_plane_size(struct bo *bo, size_t plane)
{
	assert(plane < bo->num_planes);
	return bo->sizes[plane];
}

uint32_t drv_bo_get_plane_stride(struct bo *bo, size_t plane)
{
	assert(plane < bo->num_planes);
	return bo->strides[plane];
}

uint64_t drv_bo_get_plane_format_modifier(struct bo *bo, size_t plane)
{
	assert(plane < bo->num_planes);
	return bo->format_modifiers[plane];
}

uint32_t drv_bo_get_format(struct bo *bo)
{
	return bo->format;
}

uint32_t drv_resolve_format(struct driver *drv, uint32_t format, uint64_t use_flags)
{
	if (drv->backend->resolve_format)
		return drv->backend->resolve_format(format, use_flags);

	return format;
}

size_t drv_num_planes_from_format(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_ABGR1555:
	case DRM_FORMAT_ABGR2101010:
	case DRM_FORMAT_ABGR4444:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_ARGB1555:
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_ARGB4444:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_AYUV:
	case DRM_FORMAT_BGR233:
	case DRM_FORMAT_BGR565:
	case DRM_FORMAT_BGR888:
	case DRM_FORMAT_BGRA1010102:
	case DRM_FORMAT_BGRA4444:
	case DRM_FORMAT_BGRA5551:
	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_BGRX1010102:
	case DRM_FORMAT_BGRX4444:
	case DRM_FORMAT_BGRX5551:
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_C8:
	case DRM_FORMAT_GR88:
	case DRM_FORMAT_R8:
	case DRM_FORMAT_RG88:
	case DRM_FORMAT_RGB332:
	case DRM_FORMAT_RGB565:
	case DRM_FORMAT_RGB888:
	case DRM_FORMAT_RGBA1010102:
	case DRM_FORMAT_RGBA4444:
	case DRM_FORMAT_RGBA5551:
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_RGBX1010102:
	case DRM_FORMAT_RGBX4444:
	case DRM_FORMAT_RGBX5551:
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_UYVY:
	case DRM_FORMAT_VYUY:
	case DRM_FORMAT_XBGR1555:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_XBGR4444:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_XRGB1555:
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_XRGB4444:
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_YUYV:
	case DRM_FORMAT_YVYU:
		return 1;
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
		return 2;
	case DRM_FORMAT_YVU420:
	case DRM_FORMAT_YVU420_ANDROID:
		return 3;
	}

	return i915_private_num_planes_from_format(format);
}

uint32_t drv_num_buffers_per_bo(struct bo *bo)
{
	uint32_t count = 0;
	size_t plane, p;

	for (plane = 0; plane < bo->num_planes; plane++) {
		for (p = 0; p < plane; p++)
			if (bo->handles[p].u32 == bo->handles[plane].u32)
				break;
		if (p == plane)
			count++;
	}

	return count;
}



/* Picture aspect ratio flags */
#define DRM_MODE_FLAG_PARMASK    (3<<19)
#define DRM_MODE_FLAG_PARNONE    (0<<19)
#define DRM_MODE_FLAG_PAR16_9    (1<<19)
#define DRM_MODE_FLAG_PAR4_3     (2<<19)

static const char *connector_type_names[] = {
    "None",
    "VGA",
    "DVI",
    "DVI",
    "DVI",
    "Composite",
    "TV",
    "LVDS",
    "CTV",
    "DIN",
    "DP",
    "HDMI",
    "HDMI",
    "TV",
    "eDP",
    "VIRTUAL",
    "DSI",
};



static drmModeConnectorPtr fetch_connector(kms_t* kms, uint32_t type, uint32_t type_id)
{
    int i;

    ALOGD("%s, type = %d, type_id = %d", __func__, type, type_id);

    if (!kms->resources)
        return NULL;

    for (i = 0; i < kms->resources->count_connectors; i++) {
        drmModeConnectorPtr connector =
            connector = drmModeGetConnector(kms->fd,
                                            kms->resources->connectors[i]);                       
        if (connector) {
            if ( (connector->connector_type == type) &&
                 (connector->connector_type_id == type_id) && 
                 (connector->connection == DRM_MODE_CONNECTED) ) {
                ALOGD("%s, found connector[%d], id = 0x%x!", __func__, i, connector->connector_id);                 
                return connector;
            }
            drmModeFreeConnector(connector);
        }
    }
    return NULL;
}

static int find_crtc_for_connector(kms_t* kms, drmModeConnector *connector)
{
    drmModeEncoder *encoder;
    uint32_t possible_crtcs;
    int i, j;
    uint32_t lxc_id = 1;

    for (j = 0; j < connector->count_encoders; j++) {
        encoder = drmModeGetEncoder(kms->fd, connector->encoders[j]);
        if (encoder == NULL) {
            printf("Failed to get encoder.\n");
            return -1;
        }
        possible_crtcs = encoder->possible_crtcs;
        drmModeFreeEncoder(encoder);

        for (i = 0; i < kms->resources->count_crtcs; i++) {
            if ( (possible_crtcs & (1 << i)) &&
                 !(kms->crtc_allocator & (1 << i)) ) {
                 if (lxc_id == kms->lxc_id) {
                    return kms->resources->crtcs[i];
                 }
                 else {
                    lxc_id++;
                 }
                 
            }                
        }
    }

    return -1;
}


#define MARGIN_PERCENT 1.8   /* % of active vertical image*/
#define CELL_GRAN 8.0   /* assumed character cell granularity*/
#define MIN_PORCH 1 /* minimum front porch   */
#define V_SYNC_RQD 3 /* width of vsync in lines   */
#define H_SYNC_PERCENT 8.0   /* width of hsync as % of total line */
#define MIN_VSYNC_PLUS_BP 550.0 /* min time of vsync + back porch (microsec) */
#define M 600.0 /* blanking formula gradient */
#define C 40.0  /* blanking formula offset   */
#define K 128.0 /* blanking formula scaling factor   */
#define J 20.0  /* blanking formula scaling factor   */
/* C' and M' are part of the Blanking Duty Cycle computation */
#define C_PRIME   (((C - J) * K / 256.0) + J)
#define M_PRIME   (K / 256.0 * M)

static drmModeModeInfoPtr generate_mode(int h_pixels, int v_lines, float freq)
{
    float h_pixels_rnd;
    float v_lines_rnd;
    float v_field_rate_rqd;
    float top_margin;
    float bottom_margin;
    float interlace;
    float h_period_est;
    float vsync_plus_bp;
    float v_back_porch;
    float total_v_lines;
    float v_field_rate_est;
    float h_period;
    float v_field_rate;
    float v_frame_rate;
    float left_margin;
    float right_margin;
    float total_active_pixels;
    float ideal_duty_cycle;
    float h_blank;
    float total_pixels;
    float pixel_freq;
    float h_freq;

    float h_sync;
    float h_front_porch;
    float v_odd_front_porch_lines;
    int interlaced = 0;
    int margins = 0;

    drmModeModeInfoPtr m = malloc(sizeof(drmModeModeInfo));

    h_pixels_rnd = rint((float) h_pixels / CELL_GRAN) * CELL_GRAN;
    v_lines_rnd = interlaced ? rint((float) v_lines) / 2.0 : rint((float) v_lines);
    v_field_rate_rqd = interlaced ? (freq * 2.0) : (freq);
    top_margin = margins ? rint(MARGIN_PERCENT / 100.0 * v_lines_rnd) : (0.0);
    bottom_margin = margins ? rint(MARGIN_PERCENT / 100.0 * v_lines_rnd) : (0.0);
    interlace = interlaced ? 0.5 : 0.0;
    h_period_est = (((1.0 / v_field_rate_rqd) - (MIN_VSYNC_PLUS_BP / 1000000.0)) / (v_lines_rnd + (2 * top_margin) + MIN_PORCH + interlace) * 1000000.0);
    vsync_plus_bp = rint(MIN_VSYNC_PLUS_BP / h_period_est);
    v_back_porch = vsync_plus_bp - V_SYNC_RQD;
    total_v_lines = v_lines_rnd + top_margin + bottom_margin + vsync_plus_bp + interlace + MIN_PORCH;
    v_field_rate_est = 1.0 / h_period_est / total_v_lines * 1000000.0;
    h_period = h_period_est / (v_field_rate_rqd / v_field_rate_est);
    v_field_rate = 1.0 / h_period / total_v_lines * 1000000.0;
    v_frame_rate = interlaced ? v_field_rate / 2.0 : v_field_rate;
    left_margin = margins ? rint(h_pixels_rnd * MARGIN_PERCENT / 100.0 / CELL_GRAN) * CELL_GRAN : 0.0;
    right_margin = margins ? rint(h_pixels_rnd * MARGIN_PERCENT / 100.0 / CELL_GRAN) * CELL_GRAN : 0.0;
    total_active_pixels = h_pixels_rnd + left_margin + right_margin;
    ideal_duty_cycle = C_PRIME - (M_PRIME * h_period / 1000.0);
    h_blank = rint(total_active_pixels * ideal_duty_cycle / (100.0 - ideal_duty_cycle) / (2.0 * CELL_GRAN)) * (2.0 * CELL_GRAN);
    total_pixels = total_active_pixels + h_blank;
    pixel_freq = total_pixels / h_period;
    h_freq = 1000.0 / h_period;
    h_sync = rint(H_SYNC_PERCENT / 100.0 * total_pixels / CELL_GRAN) * CELL_GRAN;
    h_front_porch = (h_blank / 2.0) - h_sync;
    v_odd_front_porch_lines = MIN_PORCH + interlace;

    m->clock = ceil(pixel_freq) * 1000;
    m->hdisplay = (int) (h_pixels_rnd);
    m->hsync_start = (int) (h_pixels_rnd + h_front_porch);
    m->hsync_end = (int) (h_pixels_rnd + h_front_porch + h_sync);
    m->htotal = (int) (total_pixels);
    m->hskew = 0;
    m->vdisplay = (int) (v_lines_rnd);
    m->vsync_start = (int) (v_lines_rnd + v_odd_front_porch_lines);
    m->vsync_end = (int) (int) (v_lines_rnd + v_odd_front_porch_lines + V_SYNC_RQD);
    m->vtotal = (int) (total_v_lines);
    m->vscan = 0;
    m->vrefresh = freq;
    m->flags = 10;
    m->type = 64;

    return (m);
}

static drmModeModeInfoPtr find_mode(drmModeConnectorPtr connector, int *bpp)
{
    char value[PROPERTY_VALUE_MAX];
    drmModeModeInfoPtr mode;
    int dist, i;
    int xres = 0, yres = 0, rate = 0;
    int forcemode = 0;
    char property[256];

    snprintf(property, 256, "debug.kms.%s.mode", (connector_type_names[connector->connector_type]));
    if (property_get(property, value, NULL)) {
        char *p = value, *end;

        /* parse <xres>x<yres>[@<bpp>] */
        if (sscanf(value, "%dx%d@%d", &xres, &yres, bpp) != 3) {
            *bpp = 0;
            if (sscanf(value, "%dx%d", &xres, &yres) != 2)
                xres = yres = 0;
        }

        if ((xres && yres) || *bpp) {
            ALOGI("will find the closest match for %dx%d@%d",
                  xres, yres, *bpp);
        }
    } else if (property_get("debug.kms.mode.force", value, NULL)) {
        char *p = value, *end;
        *bpp = 0;

        /* parse <xres>x<yres>[@<refreshrate>] */
        if (sscanf(value, "%dx%d@%d", &xres, &yres, &rate) != 3) {
            rate = 60;
            if (sscanf(value, "%dx%d", &xres, &yres) != 2)
                xres = yres = 0;
        }

        if (xres && yres && rate) {
            ALOGI("will use %dx%d@%dHz", xres, yres, rate);
            forcemode = 1;
        }
    } else {
        *bpp = 0;
    }

    dist = INT_MAX;

    if (forcemode)
        mode = generate_mode(xres, yres, rate);
    else {
        mode = NULL;
        for (i = 0; i < connector->count_modes; i++) {
            drmModeModeInfoPtr m = &connector->modes[i];
            int tmp;

            if (xres && yres) {
                tmp = (m->hdisplay - xres) * (m->hdisplay - xres) +
                      (m->vdisplay - yres) * (m->vdisplay - yres);
            }
            else {
                /* use the first preferred mode */
                tmp = (m->type & DRM_MODE_TYPE_PREFERRED) ? 0 : dist;
            }

            if (tmp < dist) {
                mode = m;
                dist = tmp;
                if (!dist)
                    break;
            }
        }
    }

    /* fallback to the first mode */
    if (!mode)
        mode = &connector->modes[0];

    /* Fix HDMI cert 7.27 AVI Info_Frame VIC(video code) failure to support 16:9 */
    mode->flags |= DRM_MODE_FLAG_PAR16_9;

    ALOGI("Established mode:");
    ALOGI("clock: %d, hdisplay: %d, hsync_start: %d, hsync_end: %d, htotal: %d, hskew: %d", mode->clock, mode->hdisplay, mode->hsync_start, mode->hsync_end, mode->htotal, mode->hskew);
    ALOGI("vdisplay: %d, vsync_start: %d, vsync_end: %d, vtotal: %d, vscan: %d, vrefresh: %d", mode->vdisplay, mode->vsync_start, mode->vsync_end, mode->vtotal, mode->vscan, mode->vrefresh);
    ALOGI("flags: %d, type: %d, name %s", mode->flags, mode->type, mode->name);

    *bpp /= 8;

    return mode;
}


static int kms_init_with_connector(kms_t* kms, kms_output_t* output, drmModeConnectorPtr connector)
{
    drmModeEncoderPtr encoder;
    drmModeModeInfoPtr mode;
    static int used_crtcs = 0;
    int bpp, i;
    int lxc_id = 1;

    if (!connector->count_modes)
        return -EINVAL;

    encoder = drmModeGetEncoder(kms->fd, connector->encoders[0]);
    if (!encoder)
        return -EINVAL;

    /* find first possible crtc which is not used yet */
    for (i = 0; i < kms->resources->count_crtcs; i++) {
        if ( (encoder->possible_crtcs & (1 << i)) &&
             ((used_crtcs & (1 << i)) != (1 << i)) ) {
                if(lxc_id >= kms->lxc_id) {                    
                    break;                    
                }
                else {
                    lxc_id++;
                }
            }
    }

    used_crtcs |= (1 << i);

    ALOGI("i = %d, used_crtcs = %x", i, used_crtcs);

    drmModeFreeEncoder(encoder);
    if (i == kms->resources->count_crtcs)
        return -EINVAL;

    output->crtc_id = find_crtc_for_connector(kms, connector);
    output->connector_id = connector->connector_id;
    output->pipe = i;
    kms->crtc_allocator |= (1 << output->crtc_id);

    /* print connector info */
#if 1    
    if (connector->count_modes > 1) {
        ALOGI("there are %d modes on connector 0x%x, type %d",
              connector->count_modes,
              connector->connector_id,
              connector->connector_type);
        for (i = 0; i < connector->count_modes; i++)
            ALOGI("  %s", connector->modes[i].name);
    }
    else {
        ALOGI("there is one mode on connector 0x%d: %s",
              connector->connector_id,
              connector->modes[0].name);
    }
#endif
    mode = find_mode(connector, &bpp);

    ALOGI("the best mode is %s", mode->name);

    output->mode = *mode;
    switch (bpp) {
    case 2:
        output->fb_format = HAL_PIXEL_FORMAT_RGB_565;
        break;
    case 4:
    default:
        output->fb_format = HAL_PIXEL_FORMAT_BGRA_8888;
        //output->fb_format = HAL_PIXEL_FORMAT_RGBA_8888;
        break;
    }

    if (connector->mmWidth && connector->mmHeight) {
        output->xdpi = (output->mode.hdisplay * 25.4 / connector->mmWidth);
        output->ydpi = (output->mode.vdisplay * 25.4 / connector->mmHeight);
    }
    else {
        output->xdpi = 75;
        output->ydpi = 75;
    }
    
    output->swap_interval = 1;

    return 0;
}


#if USE_DRM_BXT
#define LXC_INSTANCE_NO     1
#define LXC_SHIFT_DISPLAY   0
#else 
#define LXC_INSTANCE_NO     0
#define LXC_SHIFT_DISPLAY   1
#endif 

#if LXC_INSTANCE_NO
#include "devns_ctl.h"
//#include <lxc-mtab/devns_ctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
int devns_get_context(struct dnc_context_struct *cxt)
{
        int fd;
        int ret;
        if (cxt == NULL){
                ALOGW("XELATEX - %s : Error cxt.", __func__);
                return -1;
        }
        fd = open(DEV_PATH "/" DNC_NAME, O_WRONLY);
        if (fd<0){
                ALOGW("XELATEX - %s : Open dnc device failed.", __func__);
                return -1;
        }
        ret = ioctl(fd, DNC_GET_CONTEXT, cxt);
        if (ret < 0){
                ALOGW("XELATEX - %s : dnc ioctl failed.", __func__);
                close(fd);
                return -1;
        }
        close(fd);
        return 0;
}

#endif


static int kms_wait_vblank(kms_t *kms)
{
    drmVBlank vbl;
    int ret = 0;
    
    memset(&vbl, 0, sizeof(vbl));
    vbl.request.type = DRM_VBLANK_RELATIVE;
    vbl.request.sequence = 1;

    ret = drmWaitVBlank(kms->fd, &vbl);
    if (ret) {
        ALOGW("%s : %d : wait vblank failed, error is (%s)", __func__, __LINE__, strerror(errno));
    }

    return ret;
}



int drv_init_kms(struct driver* drv)
{
    drmModeConnectorPtr lvds, hdmi, edp;
    uint32_t type_id = 1;
    int connected_count = 0;
    int i, ret;

    ALOGI("%s: %d, drv = 0x%08x", __func__, __LINE__, drv);

    kms_t* kms = &(drv->kms);

    kms->fd = drv->fd;
    
    if (kms->resources)
        return 0;

    kms->resources = drmModeGetResources(kms->fd);
    if (!kms->resources) {
        ALOGE("failed to get modeset resources");
        return -EINVAL;
    }

    kms->plane_resources = drmModeGetPlaneResources(kms->fd);
    if (!kms->plane_resources) {
        ALOGE("no planes found from drm resources");
    } else {
        unsigned int i, j;
        ALOGE("found %d drm planes", kms->plane_resources->count_planes);
        kms->planes = calloc(kms->plane_resources->count_planes, sizeof(drmModePlanePtr));
        for (i = 0; i < kms->plane_resources->count_planes; i++) {
            kms->planes[i] = drmModeGetPlane(drv->fd, kms->plane_resources->planes[i]);
        }
    }

#if LXC_INSTANCE_NO
    {
        struct dnc_context_struct cxt;
        int err;
        err = devns_get_context(&cxt);
        if (err < 0){
            ALOGW("XELATEX - %s : devns_get_context failed.", __func__);
            kms->lxc_id = 1;
        }else {
            ALOGW("cxt.process_devns_id for %d\n", cxt.process_devns_id);
            kms->lxc_id = cxt.process_devns_id;
        }
    }
#else
    {
        char prop_id[PROPERTY_VALUE_MAX];
        memset(prop_id, 0, sizeof(prop_id));
        property_get("sys.container.id", prop_id, "0");
        kms->lxc_id  = atoi(prop_id) + 1;
    }
#endif
    ALOGD("LXC id = %d", kms->lxc_id);    

    {
        uint32_t connector_id;
        uint32_t encoder_id;
        uint32_t connector_type;
        uint32_t connector_type_id;

        ALOGD("count_connectors = %d", kms->resources->count_connectors);     

        for (i = 0; i < kms->resources->count_connectors; i++) {
            drmModeConnectorPtr connector;
            connector = drmModeGetConnector(kms->fd, kms->resources->connectors[i]);

            if (connector) {
                ALOGD("connector %d : connector_id = %x, encoder_id = %x, connector_type = %d, connector_type_id = %d, connection = %d",
                    i, connector->connector_id, connector->encoder_id, connector->connector_type, connector->connector_type_id, connector->connection);

                if(connector->connection == DRM_MODE_CONNECTED) {
                    connected_count++;
                }

                drmModeFreeConnector(connector);
            }
        }
    }

    if(connected_count > 1) {
        kms->lxc_id = kms->lxc_id % connected_count + LXC_SHIFT_DISPLAY;
        ALOGD("LXC id = %d after shift", kms->lxc_id);
    }

    /* find the crtc/connector/mode to use */
    lvds = fetch_connector(kms, DRM_MODE_CONNECTOR_LVDS, 1);
    if (lvds) {
        ALOGI("init primary with LVDS");
        kms_init_with_connector(kms, &kms->primary, lvds);
        drmModeFreeConnector(lvds);
        kms->primary.active = 1;
    }

    kms->edp_available = 0;
    type_id = kms->lxc_id;

    edp = fetch_connector(kms, DRM_MODE_CONNECTOR_DisplayPort, type_id);
    if(!edp) {
        edp = fetch_connector(kms, DRM_MODE_CONNECTOR_DisplayPort, type_id+1);
    }
    if(!edp) {
        edp = fetch_connector(kms, DRM_MODE_CONNECTOR_DisplayPort, type_id+2);
    }

    if(!edp) {
        edp = fetch_connector(kms, DRM_MODE_CONNECTOR_eDP, type_id);
    }
    if(!edp) {
        edp = fetch_connector(kms, DRM_MODE_CONNECTOR_eDP, type_id+1);
    }
    if(!edp) {
        edp = fetch_connector(kms, DRM_MODE_CONNECTOR_eDP, type_id+2);
    }

    if (edp) {
        kms->edp_available = 1;
        ALOGI("init primary with eDP/DP");
        kms_init_with_connector(kms, &kms->primary, edp);
        drmModeFreeConnector(edp);
        kms->primary.active = 1;
    }


    /* if still no connector, find first connected connector and try it */
    if (!kms->primary.active) {
        if(kms->edp_available) {
            type_id = kms->lxc_id - 1;
        }
        else {
#if USE_DRM_BXT
            // Non AAAG eDP is always there,  type id = lxc_id -1
            type_id = kms->lxc_id - 1;
#else
            type_id = kms->lxc_id;
#endif
        }
        ALOGD("search connected connector with type_id = %d", type_id);


        for (i = 0; i < kms->resources->count_connectors; i++) {
            drmModeConnectorPtr connector;

            connector = drmModeGetConnector(kms->fd,
                                            kms->resources->connectors[i]);
            if (connector) {
                if ((connector->connection == DRM_MODE_CONNECTED) && 
                    (connector->connector_type == DRM_MODE_CONNECTOR_HDMIA) && 
                    (connector->connector_type_id  == type_id) ) {
                    if (!kms_init_with_connector(kms, &kms->primary, connector)) {
                        ALOGD("first connector is the primary connector");                        
                        break;
                    }
                }

                drmModeFreeConnector(connector);
            }
        }

        if (i == kms->resources->count_connectors) {
            ALOGE("failed to find a valid crtc/connector/mode combination");
            drmModeFreeResources(kms->resources);
            kms->resources = NULL;
            return -EINVAL;
        }
    }


    /* check if hdmi is connected already */
    if (kms->lxc_id > 1) {   
#if USE_DRM_BXT
         // Non AAAG eDP is always there,  type id = lxc_id -1
        type_id = kms->lxc_id - 1;
#else
        type_id = kms->lxc_id;
#endif         
        hdmi = fetch_connector(kms, DRM_MODE_CONNECTOR_HDMIA, type_id);
        if (!hdmi) {
            hdmi = fetch_connector(kms, DRM_MODE_CONNECTOR_HDMIB, type_id);
        }

        if (hdmi) {
            if (hdmi->connector_id == kms->primary.connector_id) {
                /* special case: our primary connector is hdmi */
                ALOGD("hdmi is the primary connector");
                goto skip_hdmi_modes;
            }

            drmModeFreeConnector(hdmi);
        }
   }
   ALOGD("primary output crtc = %d, connector = %d, pipe = %d",
         kms->primary.crtc_id, kms->primary.connector_id, kms->primary.pipe);

skip_hdmi_modes:

    kms->first_post = 1;
    return 0;
}

void drv_fini_kms(struct driver* drv)
{
    kms_t* kms = &(drv->kms);

    /* wait previous flip complete */
    // kms_page_flip(drm, NULL);

    /* restore crtc? */


    if (kms->planes) {
        unsigned int i;
        for (i = 0; i < kms->plane_resources->count_planes; i++)
            drmModeFreePlane(kms->planes[i]);
        free(kms->planes);
        kms->planes = NULL;
    }

    if (kms->plane_resources) {
        drmModeFreePlaneResources(kms->plane_resources);
        kms->plane_resources = NULL;
    }

    if (kms->resources) {
        drmModeFreeResources(kms->resources);
        kms->resources = NULL;
    }

    memset(kms, 0, sizeof(kms_t));
}

    
int drv_get_kms_info(struct driver* drv, kms_info_t *info)
{
    kms_t* kms = &(drv->kms);
    
    info->flags             = 0x1;
    info->width             = kms->primary.mode.hdisplay;
    info->height            = kms->primary.mode.vdisplay;
    info->stride            = kms->primary.mode.hdisplay;
    info->fps               = kms->primary.mode.vrefresh;
    info->format            = kms->primary.fb_format;
    info->xdpi              = kms->primary.xdpi;
    info->ydpi              = kms->primary.ydpi;
    info->minSwapInterval   = kms->primary.swap_interval;
    info->maxSwapInterval   = kms->primary.swap_interval;
    info->numFramebuffers   = 3;
    return 0;
}


int drv_present_bo(struct driver* drv, struct bo *bo)
{
    kms_t* kms = &(drv->kms);
    int ret = 0;

    if(!bo->fb_id) {
        uint32_t gem_handles[DRV_MAX_PLANES] = {0};
        int i = 0;

        for (i = 0; i < DRV_MAX_PLANES; i++) {
            gem_handles[i] = bo->handles[i].u32;
        }
        
        ret = drmModeAddFB2(kms->fd, bo->width, bo->height, bo->format, 
                            gem_handles, bo->strides, bo->offsets, &(bo->fb_id), 0);
        if(ret) {
            ALOGE("%s : %d : add fb failed %d", __func__, __LINE__, ret);
        }
    }

    if(bo->fb_id) {
        
        if(kms->first_post) {
            ALOGI("%s : %d : set crtc (crtc id = %d, fb_id = %d, connector_id = %d)",
                  __func__, __LINE__, kms->primary.crtc_id, bo->fb_id, kms->primary.connector_id);

            ret = drmModeSetCrtc(kms->fd, kms->primary.crtc_id, bo->fb_id,
                                 0, 0, &(kms->primary.connector_id), 1, &(kms->primary.mode));
            if (ret) {
                ALOGE("%s : %d : failed to set crtc (crtc id = %d, fb_id = %d, connector_id = %d)",
                      __func__, __LINE__, kms->primary.crtc_id, bo->fb_id, kms->primary.connector_id);
            }
        }

        if (!ret) {
            kms->first_post = 0;
            kms->front_bo   = bo;
            kms->back_bo    = NULL;
        }            

        
        // 
        if(!ret) {
            uint32_t flags = 0;
            ret = drmModePageFlip(kms->fd, kms->primary.crtc_id, bo->fb_id, flags, (void *)kms);        
            if (ret) {
                if(ret != -EBUSY) {
                    ALOGE("%s : %d : page flip failed (crtc_id = %d, fb_id =%d, flags = %d), ret = %d, error = %s)",
                          __func__, __LINE__, kms->primary.crtc_id, bo->fb_id, flags, ret, strerror(errno));
                    kms->first_post = 1;
                }
            }
            else {
                kms->back_bo = bo;
            }            
        }

        if(!ret) {
            ret = kms_wait_vblank(kms);
            if(!ret) {
                kms->front_bo = bo;
                kms->back_bo = NULL;
            }
            else {
                kms->first_post = 1;
            }
        }
    }

    
    return ret;
}


