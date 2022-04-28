/*
 * Copyright 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifdef DRV_I915

#include <errno.h>
#include <i915_drm.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <xf86drm.h>

#include <cutils/log.h>
#include <cutils/properties.h>

#include "drv_priv.h"
#include "helpers.h"
#include "util.h"
#include "i915_private.h"

#define I915_CACHELINE_SIZE 64
#define I915_CACHELINE_MASK (I915_CACHELINE_SIZE - 1)
/* Mmap offset ioctl */
#define I915_PARAM_MMAP_OFFSET_VERSION  55
#define DRM_I915_GEM_MMAP_OFFSET       DRM_I915_GEM_MMAP_GTT
#define DRM_IOCTL_I915_GEM_MMAP_OFFSET         DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_MMAP_OFFSET, struct drm_i915_gem_mmap_offset)
#define DRM_I915_QUERY_MEMORY_REGIONS   4

enum drm_i915_gem_memory_class {
	I915_MEMORY_CLASS_SYSTEM = 0,
	I915_MEMORY_CLASS_DEVICE,
	I915_MEMORY_CLASS_STOLEN_SYSTEM,
	I915_MEMORY_CLASS_STOLEN_DEVICE,
};

#define DRM_IOCTL_I915_QUERY DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_QUERY, struct drm_i915_query)
#define DRM_IOCTL_I915_GEM_CREATE                                                                  \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_CREATE, struct drm_i915_gem_create)
#define DRM_IOCTL_I915_GEM_CREATE_EXT                                                              \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_CREATE, struct drm_i915_gem_create_ext)

struct drm_i915_gem_create_ext {

	/**
	 * Requested size for the object.
	 *
	 * The (page-aligned) allocated size for the object will be returned.
	 */
	__u64 size;
	/**
	 * Returned handle for the object.
	 *
	 * Object handles are nonzero.
	 */
	__u32 handle;
	__u32 pad;
#define I915_GEM_CREATE_EXT_SETPARAM (1u << 0)
#define I915_GEM_CREATE_EXT_FLAGS_UNKNOWN (-(I915_GEM_CREATE_EXT_SETPARAM << 1))
	__u64 extensions;
};

struct drm_i915_gem_memory_class_instance {
	__u16 memory_class; /* see enum drm_i915_gem_memory_class */
	__u16 memory_instance;
};

struct drm_i915_memory_region_info {
	/** class:instance pair encoding */
	struct drm_i915_gem_memory_class_instance region;

	/** MBZ */
	__u32 rsvd0;

	/** MBZ */
	__u64 caps;

	/** MBZ */
	__u64 flags;

	/** Memory probed by the driver (-1 = unknown) */
	__u64 probed_size;

	/** Estimate of memory remaining (-1 = unknown) */
	__u64 unallocated_size;

	/** MBZ */
	__u64 rsvd1[8];
};

struct drm_i915_gem_object_param {
	/* Object handle (0 for I915_GEM_CREATE_EXT_SETPARAM) */
	__u32 handle;

	/* Data pointer size */
	__u32 size;

/*
 * I915_OBJECT_PARAM:
 *
 * Select object namespace for the param.
 */
#define I915_OBJECT_PARAM (1ull << 32)

/*
 * I915_PARAM_MEMORY_REGIONS:
 *
 * Set the data pointer with the desired set of placements in priority
 * order(each entry must be unique and supported by the device), as an array of
 * drm_i915_gem_memory_class_instance, or an equivalent layout of class:instance
 * pair encodings. See DRM_I915_QUERY_MEMORY_REGIONS for how to query the
 * supported regions.
 *
 * Note that this requires the I915_OBJECT_PARAM namespace:
 *	.param = I915_OBJECT_PARAM | I915_PARAM_MEMORY_REGIONS
 */
#define I915_PARAM_MEMORY_REGIONS 0x1
	__u64 param;

	/* Data value or pointer */
	__u64 data;
};

struct drm_i915_gem_create_ext_setparam {
	struct i915_user_extension base;
	struct drm_i915_gem_object_param param;
};

struct drm_i915_query_memory_regions {
	/** Number of supported regions */
	__u32 num_regions;

	/** MBZ */
	__u32 rsvd[3];

	/* Info about each supported region */
	struct drm_i915_memory_region_info regions[];
};

static const uint32_t render_target_formats[] = { DRM_FORMAT_ABGR8888,	  DRM_FORMAT_ARGB1555,
						  DRM_FORMAT_ARGB8888,	  DRM_FORMAT_RGB565,
						  DRM_FORMAT_XBGR2101010, DRM_FORMAT_XBGR8888,
						  DRM_FORMAT_XRGB1555,	  DRM_FORMAT_XRGB2101010,
						  DRM_FORMAT_XRGB8888 };

static const uint32_t tileable_texture_source_formats[] = { DRM_FORMAT_GR88, DRM_FORMAT_R8,
							    DRM_FORMAT_UYVY, DRM_FORMAT_YUYV,
							    DRM_FORMAT_YVYU, DRM_FORMAT_VYUY };

static const uint32_t texture_source_formats[] = { DRM_FORMAT_YVU420, DRM_FORMAT_YVU420_ANDROID,
						   DRM_FORMAT_NV12 };
struct drm_i915_gem_mmap_offset {
        /** Handle for the object being mapped. */
        __u32 handle;
        __u32 pad;
        /**
         * Fake offset to use for subsequent mmap call
         *
         * This is a fixed-size type for 32/64 compatibility.
         */
        __u64 offset;

        /**
         * Flags for extended behaviour.
         *
         * It is mandatory that either one of the _WC/_WB flags
         * should be passed here.
         */
        __u64 flags;
#define I915_MMAP_OFFSET_WC (1 << 0)
#define I915_MMAP_OFFSET_WB (1 << 1)
#define I915_MMAP_OFFSET_UC (1 << 2)
#define I915_MMAP_OFFSET_FLAGS (I915_MMAP_OFFSET_WC | I915_MMAP_OFFSET_WB | I915_MMAP_OFFSET_UC)
};

struct iris_memregion {
	struct drm_i915_gem_memory_class_instance region;
	uint64_t size;
};

struct i915_device
{
	uint32_t gen;
	int32_t has_llc;
	uint64_t cursor_width;
	uint64_t cursor_height;
	int32_t has_mmap_offset;
	struct iris_memregion vram, sys;
};

static uint32_t i915_get_gen(int device_id)
{
        ALOGI("%s : %d : device_id = %x", __func__, __LINE__, device_id);
	const uint16_t gen3_ids[] = { 0x2582, 0x2592, 0x2772, 0x27A2, 0x27AE,
				      0x29C2, 0x29B2, 0x29D2, 0xA001, 0xA011 };
        const uint16_t gen12_ids[] = { 0x9A40, 0x9A49, 0x9A59, 0x9A60, 0x9A68,
                                       0x9A70, 0x9A78, 0xFF20, 0x4905, 0x4906,
                                       0x4907, 0x4f80, 0x0201, 0xFF25 };
	unsigned i;
	for (i = 0; i < ARRAY_SIZE(gen3_ids); i++)
		if (gen3_ids[i] == device_id)
			return 3;
	for (i = 0; i < ARRAY_SIZE(gen12_ids); i++)
		if (gen12_ids[i] == device_id)
			return 12;

	return 4;
}

static int i915_add_kms_item(struct driver *drv, const struct kms_item *item)
{
	uint32_t i;
	struct combination *combo;

	/*
	 * Older hardware can't scanout Y-tiled formats. Newer devices can, and
	 * report this functionality via format modifiers.
	 */
	for (i = 0; i < drv->combos.size; i++) {
		combo = &drv->combos.data[i];
		if (combo->format != item->format)
			continue;

		if (item->modifier == DRM_FORMAT_MOD_INVALID &&
		    combo->metadata.tiling == I915_TILING_X) {
			/*
			 * FIXME: drv_query_kms() does not report the available modifiers
			 * yet, but we know that all hardware can scanout from X-tiled
			 * buffers, so let's add this to our combinations, except for
			 * cursor, which must not be tiled.
			 */
			combo->use_flags |= item->use_flags & ~BO_USE_CURSOR;
		}

		if (combo->metadata.modifier == item->modifier)
			combo->use_flags |= item->use_flags;
	}

	return 0;
}

static int i915_add_combinations(struct driver *drv)
{
	int ret;
	uint32_t i, num_items;
	struct kms_item *items;
	struct format_metadata metadata;
	uint64_t render_use_flags, texture_use_flags;

	render_use_flags = BO_USE_RENDER_MASK;
	texture_use_flags = BO_USE_TEXTURE_MASK;

	metadata.tiling = I915_TILING_NONE;
	metadata.priority = 1;
	metadata.modifier = DRM_FORMAT_MOD_LINEAR;

	ret = drv_add_combinations(drv, render_target_formats, ARRAY_SIZE(render_target_formats),
				   &metadata, render_use_flags);
	if (ret)
		return ret;

	ret = drv_add_combinations(drv, texture_source_formats, ARRAY_SIZE(texture_source_formats),
				   &metadata, texture_use_flags);
	if (ret)
		return ret;

	ret = drv_add_combinations(drv, tileable_texture_source_formats,
				   ARRAY_SIZE(tileable_texture_source_formats), &metadata,
				   texture_use_flags);
	if (ret)
		return ret;

	drv_modify_combination(drv, DRM_FORMAT_XRGB8888, &metadata, BO_USE_CURSOR | BO_USE_SCANOUT);
	drv_modify_combination(drv, DRM_FORMAT_ARGB8888, &metadata, BO_USE_CURSOR | BO_USE_SCANOUT);
	drv_modify_combination(drv, DRM_FORMAT_ABGR8888, &metadata,
			       BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE);

	/* IPU3 camera ISP supports only NV12 output. */
	drv_modify_combination(drv, DRM_FORMAT_NV12, &metadata,
			       BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE);
	/*
	 * R8 format is used for Android's HAL_PIXEL_FORMAT_BLOB and is used for JPEG snapshots
	 * from camera.
	 */
	drv_modify_combination(drv, DRM_FORMAT_R8, &metadata,
			       BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE);

	render_use_flags &= ~BO_USE_RENDERSCRIPT;
	render_use_flags &= ~BO_USE_SW_WRITE_OFTEN;
	render_use_flags &= ~BO_USE_SW_READ_OFTEN;
	render_use_flags &= ~BO_USE_LINEAR;

	texture_use_flags &= ~BO_USE_RENDERSCRIPT;
	texture_use_flags &= ~BO_USE_SW_WRITE_OFTEN;
	texture_use_flags &= ~BO_USE_SW_READ_OFTEN;
	texture_use_flags &= ~BO_USE_LINEAR;

	metadata.tiling = I915_TILING_X;
	metadata.priority = 2;
	metadata.modifier = I915_FORMAT_MOD_X_TILED;

	ret = drv_add_combinations(drv, render_target_formats, ARRAY_SIZE(render_target_formats),
				   &metadata, render_use_flags);
	if (ret)
		return ret;

	ret = drv_add_combinations(drv, tileable_texture_source_formats,
				   ARRAY_SIZE(tileable_texture_source_formats), &metadata,
				   texture_use_flags);
	if (ret)
		return ret;

	metadata.tiling = I915_TILING_Y;
	metadata.priority = 3;
	metadata.modifier = I915_FORMAT_MOD_Y_TILED;

	ret = drv_add_combinations(drv, render_target_formats, ARRAY_SIZE(render_target_formats),
				   &metadata, render_use_flags);
	if (ret)
		return ret;

	ret = drv_add_combinations(drv, tileable_texture_source_formats,
				   ARRAY_SIZE(tileable_texture_source_formats), &metadata,
				   texture_use_flags);
	if (ret)
		return ret;

	i915_private_add_combinations(drv);

	items = drv_query_kms(drv, &num_items);
	if (!items || !num_items)
		return 0;

	for (i = 0; i < num_items; i++) {
		ret = i915_add_kms_item(drv, &items[i]);
		if (ret) {
			free(items);
			return ret;
		}
	}

	free(items);
	return 0;
}

static int i915_align_dimensions(struct bo *bo, uint32_t tiling, uint64_t modifier,
				 uint32_t *stride, uint32_t *aligned_height)
{
	struct i915_device *i915 = bo->drv->priv;
	uint32_t horizontal_alignment = 4;
	uint32_t vertical_alignment = 4;

	switch (tiling) {
	default:
	case I915_TILING_NONE:
		horizontal_alignment = 64;
		if (modifier == I915_FORMAT_MOD_Yf_TILED ||
		    modifier == I915_FORMAT_MOD_Yf_TILED_CCS) {
			horizontal_alignment = 128;
			vertical_alignment = 32;
		}
		break;
	case I915_TILING_X:
		horizontal_alignment = 512;
		vertical_alignment = 8;
		break;
	case I915_TILING_Y:
		if (i915->gen == 3) {
			horizontal_alignment = 512;
			vertical_alignment = 8;
		} else {
                       horizontal_alignment = 128;
                       vertical_alignment = 32;
		}
		break;
	}

	/*
	 * The alignment calculated above is based on the full size luma plane and to have chroma
	 * planes properly aligned with subsampled formats, we need to multiply luma alignment by
	 * subsampling factor.
	 */
	switch (bo->format) {
	case DRM_FORMAT_YVU420_ANDROID:
	case DRM_FORMAT_YVU420:
		horizontal_alignment *= 2;
	/* Fall through */
	case DRM_FORMAT_NV12:
		vertical_alignment *= 2;
		break;
	}

	i915_private_align_dimensions(bo->format, &vertical_alignment);

	*aligned_height = ALIGN(bo->height, vertical_alignment);
	if (i915->gen > 3) {
		*stride = ALIGN(*stride, horizontal_alignment);
	} else {
		while (*stride > horizontal_alignment)
			horizontal_alignment <<= 1;

		*stride = horizontal_alignment;
	}

	if (i915->gen <= 3 && *stride > 8192)
		return -EINVAL;

	return 0;
}

static void i915_clflush(void *start, size_t size)
{
	void *p = (void *)(((uintptr_t)start) & ~I915_CACHELINE_MASK);
	void *end = (void *)((uintptr_t)start + size);

	__builtin_ia32_mfence();
	while (p < end) {
		__builtin_ia32_clflush(p);
		p = (void *)((uintptr_t)p + I915_CACHELINE_SIZE);
	}
}

static inline int gen_ioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, request, arg);
	} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
	return ret;
}

static int gem_param(int fd, int name)
{
	int v = -1; /* No param uses (yet) the sign bit, reserve it for errors */

	struct drm_i915_getparam gp = { .param = name, .value = &v };
	if (gen_ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp))
		return -1;

	return v;
}

#define HAVE___BUILTIN_CLZ 1

static inline unsigned
util_last_bit(unsigned u)
{
#if defined(HAVE___BUILTIN_CLZ)
   return u == 0 ? 0 : 32 - __builtin_clz(u);
#elif defined(_MSC_VER) && (_M_IX86 || _M_ARM || _M_AMD64 || _M_IA64)
   unsigned long index;
   if (_BitScanReverse(&index, u))
      return index + 1;
   else
      return 0;
#else
   unsigned r = 0;
   while (u) {
      r++;
      u >>= 1;
   }
   return r;
#endif
}

static void i915_bo_update_meminfo(struct i915_device *i915_dev,
				   const struct drm_i915_query_memory_regions *meminfo)
{
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
			break;
		default:
			break;
		}
	}
}

static bool i915_bo_query_meminfo(struct driver *drv, struct i915_device *i915_dev)
{
	struct drm_i915_query_item item = {
		.query_id = DRM_I915_QUERY_MEMORY_REGIONS,
	};

	struct drm_i915_query query = {
		.num_items = 1,
		.items_ptr = (uintptr_t)&item,
	};

	if (drmIoctl(drv->fd, DRM_IOCTL_I915_QUERY, &query))
		return false;

	struct drm_i915_query_memory_regions *meminfo = calloc(1, item.length);
        if (!meminfo) return -ENOMEM;

	item.data_ptr = (uintptr_t)meminfo;

	if (drmIoctl(drv->fd, DRM_IOCTL_I915_QUERY, &query) || item.length <= 0)
		return false;

	i915_bo_update_meminfo(i915_dev, meminfo);

	free(meminfo);

	return true;
}

static int i915_init(struct driver *drv)
{
	int ret;
	int device_id;
	struct i915_device *i915;
	drm_i915_getparam_t get_param;

	i915 = calloc(1, sizeof(*i915));
	if (!i915)
		return -ENOMEM;

	memset(&get_param, 0, sizeof(get_param));
	get_param.param = I915_PARAM_CHIPSET_ID;
	get_param.value = &device_id;
	ret = drmIoctl(drv->fd, DRM_IOCTL_I915_GETPARAM, &get_param);
	if (ret) {
		ALOGE("drv: Failed to get I915_PARAM_CHIPSET_ID\n");
		free(i915);
		return -EINVAL;
	}

	i915->gen = i915_get_gen(device_id);

	memset(&get_param, 0, sizeof(get_param));
	get_param.param = I915_PARAM_HAS_LLC;
	get_param.value = &i915->has_llc;
	ret = drmIoctl(drv->fd, DRM_IOCTL_I915_GETPARAM, &get_param);
	if (ret) {
		ALOGE("drv: Failed to get I915_PARAM_HAS_LLC\n");
		free(i915);
		return -EINVAL;
	}

	i915->has_mmap_offset = gem_param(drv->fd, I915_PARAM_MMAP_GTT_VERSION) >= 4;
	// ALOGI("%s : %d : has_mmap_offset = %d", __func__, __LINE__, i915->has_mmap_offset);

	i915_bo_query_meminfo(drv, i915);
	ALOGI("Gralloc: i915_bo_query_meminfo done");

	drv->priv = i915;

	i915_private_init(drv, &i915->cursor_width, &i915->cursor_height);

	return i915_add_combinations(drv);
}

static int i915_bo_create_for_modifier(struct bo *bo, uint32_t width, uint32_t height,
				       uint32_t format, uint64_t modifier)
{
	int ret;
	size_t plane;
	uint32_t stride;
	struct drm_i915_gem_set_tiling gem_set_tiling;
	struct i915_device *i915_dev = (struct i915_device *)bo->drv->priv;
	bo->format_modifiers[0] = modifier;
	switch (modifier) {
	case DRM_FORMAT_MOD_LINEAR:
		bo->tiling = I915_TILING_NONE;
		break;
	case I915_FORMAT_MOD_X_TILED:
		bo->tiling = I915_TILING_X;
		break;
	case I915_FORMAT_MOD_Y_TILED:
	case I915_FORMAT_MOD_Y_TILED_CCS:
	case I915_FORMAT_MOD_Yf_TILED:
	case I915_FORMAT_MOD_Yf_TILED_CCS:
		bo->tiling = I915_TILING_Y;
		break;
	}

	stride = drv_stride_from_format(format, width, 0);

	/*
	 * Align cursor width and height to values expected by Intel
	 * HW.
	 */
	if (bo->use_flags & BO_USE_CURSOR) {
		width = ALIGN(width, i915_dev->cursor_width);
		height = ALIGN(height, i915_dev->cursor_height);
		stride = drv_stride_from_format(format, width, 0);
	} else {
		ret = i915_align_dimensions(bo, bo->tiling, modifier, &stride, &height);
		if (ret)
			return ret;
	}

	/*
	 * HAL_PIXEL_FORMAT_YV12 requires the buffer height not be aligned, but we need to keep
	 * total size as with aligned height to ensure enough padding space after each plane to
	 * satisfy GPU alignment requirements.
	 *
	 * We do it by first calling drv_bo_from_format() with aligned height and
	 * DRM_FORMAT_YVU420, which allows height alignment, saving the total size it calculates
	 * and then calling it again with requested parameters.
	 *
	 * This relies on the fact that i965 driver uses separate surfaces for each plane and
	 * contents of padding bytes is not affected, as it is only used to satisfy GPU cache
	 * requests.
	 *
	 * This is enforced by Mesa in src/intel/isl/isl_gen8.c, inside
	 * isl_gen8_choose_image_alignment_el(), which is used for GEN9 and GEN8.
	 */
	if (format == DRM_FORMAT_YVU420_ANDROID) {
		uint32_t unaligned_height = bo->height;
		size_t total_size;

		drv_bo_from_format(bo, stride, height, DRM_FORMAT_YVU420);
		total_size = bo->total_size;
		drv_bo_from_format(bo, stride, unaligned_height, format);
		bo->total_size = total_size;
	} else {

		drv_bo_from_format(bo, stride, height, format);
	}

	if (modifier == I915_FORMAT_MOD_Y_TILED_CCS || modifier == I915_FORMAT_MOD_Yf_TILED_CCS) {
		/*
		 * For compressed surfaces, we need a color control surface
		 * (CCS). Color compression is only supported for Y tiled
		 * surfaces, and for each 32x16 tiles in the main surface we
		 * need a tile in the control surface.  Y tiles are 128 bytes
		 * wide and 32 lines tall and we use that to first compute the
		 * width and height in tiles of the main surface. stride and
		 * height are already multiples of 128 and 32, respectively:
		 */
		uint32_t width_in_tiles = stride / 128;
		uint32_t height_in_tiles = height / 32;

		/*
		 * Now, compute the width and height in tiles of the control
		 * surface by dividing and rounding up.
		 */
		uint32_t ccs_width_in_tiles = DIV_ROUND_UP(width_in_tiles, 32);
		uint32_t ccs_height_in_tiles = DIV_ROUND_UP(height_in_tiles, 16);
		uint32_t ccs_size = ccs_width_in_tiles * ccs_height_in_tiles * 4096;

		/*
		 * With stride and height aligned to y tiles, bo->total_size
		 * is already a multiple of 4096, which is the required
		 * alignment of the CCS.
		 */
		bo->num_planes = 2;
		bo->strides[1] = ccs_width_in_tiles * 128;
		bo->sizes[1] = ccs_size;
		bo->offsets[1] = bo->total_size;
		bo->total_size += ccs_size;
	}

	/*
	 * Quoting Mesa ISL library:
	 *
	 *    - For linear surfaces, additional padding of 64 bytes is required at
	 *      the bottom of the surface. This is in addition to the padding
	 *      required above.
	 */
	if (bo->tiling == I915_TILING_NONE)
		bo->total_size += 64;

	/*
	 * Ensure we pass aligned width/height.
	 */
	bo->aligned_width = width;
	bo->aligned_height = height;

	static bool force_mem_type = false;
	static bool force_mem_local = false;
	static bool force_mem_read = false;

	if (!force_mem_read) {
#define FORCE_MEM_PROP "sys.icr.gralloc.force_mem"
	    char mem_prop_buf[PROPERTY_VALUE_MAX];
	    if (property_get(FORCE_MEM_PROP, mem_prop_buf, "local") > 0) {
	        const char *force_mem_property = mem_prop_buf;
	        if (!strcmp(force_mem_property, "local")) {
	            force_mem_local = true; /* always use local memory */
	            force_mem_type = true;
	        } else if (!strcmp(force_mem_property, "system")) {
	            force_mem_local = false; /* always use system memory */
	            force_mem_type = true;
	        }
	    }
	    force_mem_read = true;

	    if (force_mem_type) {
	        ALOGI("Gralloc: Forcing all memory allocation to come from: %s",
	          force_mem_local ? "local" : "system");
	    }
	}

	bool local = true;
	if (force_mem_type) {
	       local = force_mem_local;
	}
	uint32_t gem_handle;
	/* If we have vram size, we have multiple memory regions and should choose
	 * one of them.
	 */
	if (i915_dev->vram.size > 0) {
		/* All new BOs we get from the kernel are zeroed, so we don't need to
		 * worry about that here.
		 */
		struct drm_i915_gem_memory_class_instance regions[2];
		uint32_t nregions = 0;
		if (local) {
			/* For vram allocations, still use system memory as a fallback. */
			regions[nregions++] = i915_dev->vram.region;
			regions[nregions++] = i915_dev->sys.region;
		} else {
			regions[nregions++] = i915_dev->sys.region;
		}

		struct drm_i915_gem_object_param region_param = {
			.size = nregions,
			.data = (uintptr_t)regions,
			.param = I915_OBJECT_PARAM | I915_PARAM_MEMORY_REGIONS,
		};

		struct drm_i915_gem_create_ext_setparam setparam_region = {
			.base = { .name = I915_GEM_CREATE_EXT_SETPARAM },
			.param = region_param,
		};

		struct drm_i915_gem_create_ext create = {
			.size = bo->total_size,
			.extensions = (uintptr_t)&setparam_region,
		};

		/* It should be safe to use GEM_CREATE_EXT without checking, since we are
		 * in the side of the branch where discrete memory is available. So we
		 * can assume GEM_CREATE_EXT is supported already.
		 */
		ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_CREATE_EXT, &create);
		if (ret) {
			ALOGE("drv: DRM_IOCTL_I915_GEM_CREATE_EXT failed (size=%llu)\n",
				  create.size);
			return ret;
		}
		gem_handle = create.handle;
	} else {
		struct drm_i915_gem_create create = { .size = bo->total_size };
		/* All new BOs we get from the kernel are zeroed, so we don't need to
		 * worry about that here.
		 */
		ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_CREATE, &create);
		if (ret) {
			ALOGE("drv: DRM_IOCTL_I915_GEM_CREATE failed (size=%llu)\n", create.size);
			return ret;
		}
		gem_handle = create.handle;
	}

	for (plane = 0; plane < bo->num_planes; plane++)
		bo->handles[plane].u32 = gem_handle;
	if (i915_dev->gen < 12) {
		memset(&gem_set_tiling, 0, sizeof(gem_set_tiling));
		gem_set_tiling.handle = bo->handles[0].u32;
		gem_set_tiling.tiling_mode = bo->tiling;
		gem_set_tiling.stride = bo->strides[0];

		ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_SET_TILING, &gem_set_tiling);
		if (ret) {
			struct drm_gem_close gem_close;
			memset(&gem_close, 0, sizeof(gem_close));
			gem_close.handle = bo->handles[0].u32;
			drmIoctl(bo->drv->fd, DRM_IOCTL_GEM_CLOSE, &gem_close);

			ALOGE("drv: DRM_IOCTL_I915_GEM_SET_TILING failed with %d", errno);
			return -errno;
		}
	}

	return 0;
}

static int i915_bo_create(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
			  uint64_t use_flags)
{
	struct combination *combo;

	combo = drv_get_combination(bo->drv, format, use_flags);
	if (!combo)
		return -EINVAL;

	return i915_bo_create_for_modifier(bo, width, height, format, combo->metadata.modifier);
}

static int i915_bo_create_with_modifiers(struct bo *bo, uint32_t width, uint32_t height,
					 uint32_t format, const uint64_t *modifiers, uint32_t count)
{
	static uint64_t modifier_order[] = {
		I915_FORMAT_MOD_Y_TILED,      I915_FORMAT_MOD_Yf_TILED, I915_FORMAT_MOD_Y_TILED_CCS,
		I915_FORMAT_MOD_Yf_TILED_CCS, I915_FORMAT_MOD_X_TILED,	DRM_FORMAT_MOD_LINEAR,
	};
	uint64_t modifier;

	modifier = drv_pick_modifier(modifiers, count, modifier_order, ARRAY_SIZE(modifier_order));

	bo->format_modifiers[0] = modifier;

	return i915_bo_create_for_modifier(bo, width, height, format, modifier);
}

static void i915_close(struct driver *drv)
{
	free(drv->priv);
	drv->priv = NULL;
}

static int i915_bo_import(struct bo *bo, struct drv_import_fd_data *data)
{
	int ret;
	struct drm_i915_gem_get_tiling gem_get_tiling;

	ret = drv_prime_bo_import(bo, data);
	if (ret)
		return ret;

	struct i915_device *i915_dev = (struct i915_device *)(bo->drv->priv);
	if (i915_dev->gen < 12) {
		/* TODO(gsingh): export modifiers and get rid of backdoor tiling. */
		memset(&gem_get_tiling, 0, sizeof(gem_get_tiling));
		gem_get_tiling.handle = bo->handles[0].u32;

		ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_GET_TILING, &gem_get_tiling);
		if (ret) {
			drv_gem_bo_destroy(bo);
			ALOGE("drv: DRM_IOCTL_I915_GEM_GET_TILING failed.");
			return ret;
		}

		bo->tiling = gem_get_tiling.tiling_mode;
	} else {
		bo->tiling = data->tiling;
	}

	return 0;
}

static void *i915_bo_map(struct bo *bo, struct map_info *data, size_t plane, uint32_t map_flags)
{
	int ret;
	void *addr;

	struct i915_device *i915 = (struct i915_device *)(bo->drv->priv);

	if (i915->has_mmap_offset) {

		bool wc = true;

		struct drm_i915_gem_mmap_offset mmap_arg = {
			.handle = bo->handles[0].u32,
			.flags = wc ? I915_MMAP_OFFSET_WC : I915_MMAP_OFFSET_WB,
		};

		/* Get the fake offset back */
		int ret = gen_ioctl(bo->drv->fd, DRM_IOCTL_I915_GEM_MMAP_OFFSET, &mmap_arg);
		if (ret != 0) {
			ALOGE("drv: DRM_IOCTL_I915_GEM_MMAP_OFFSET failed\n");
			return MAP_FAILED;
		}

		//ALOGI("%s : %d : handle = %x, size = %zd, mmpa_arg.offset = %llx", __func__,
		//      __LINE__, mmap_arg.handle, bo->total_size, mmap_arg.offset);

		/* And map it */
		addr = mmap(0, bo->total_size, PROT_READ | PROT_WRITE, MAP_SHARED, bo->drv->fd,
			    mmap_arg.offset);
	} else if (bo->tiling == I915_TILING_NONE) {
		struct drm_i915_gem_mmap gem_map;
		memset(&gem_map, 0, sizeof(gem_map));

		if ((bo->use_flags & BO_USE_SCANOUT) && !(bo->use_flags & BO_USE_RENDERSCRIPT))
			gem_map.flags = I915_MMAP_WC;

		gem_map.handle = bo->handles[0].u32;
		gem_map.offset = 0;
		gem_map.size = bo->total_size;

		ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_MMAP, &gem_map);
		if (ret) {
			ALOGE("drv: DRM_IOCTL_I915_GEM_MMAP failed\n");
			return MAP_FAILED;
		}

		addr = (void *)(uintptr_t)gem_map.addr_ptr;
	} else {
		struct drm_i915_gem_mmap_gtt gem_map;
		memset(&gem_map, 0, sizeof(gem_map));

		gem_map.handle = bo->handles[0].u32;

		ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_MMAP_GTT, &gem_map);
		if (ret) {
			ALOGE("drv: DRM_IOCTL_I915_GEM_MMAP_GTT failed\n");
			return MAP_FAILED;
		}

		addr = mmap(0, bo->total_size, drv_get_prot(map_flags), MAP_SHARED, bo->drv->fd,
			    gem_map.offset);
	}

	if (addr == MAP_FAILED) {
		ALOGE("%s : %d : i915 GEM mmap failed : %d(%s)", __func__, __LINE__, errno,
		      strerror(errno));
		return addr;
	}

	data->length = bo->total_size;
	return addr;
}

static int i915_bo_invalidate(struct bo *bo, struct map_info *data)
{
	int ret;
	struct drm_i915_gem_set_domain set_domain;

	memset(&set_domain, 0, sizeof(set_domain));
	set_domain.handle = bo->handles[0].u32;
	if (bo->tiling == I915_TILING_NONE) {
		set_domain.read_domains = I915_GEM_DOMAIN_CPU;
		if (data->map_flags & BO_MAP_WRITE)
			set_domain.write_domain = I915_GEM_DOMAIN_CPU;
	} else {
		set_domain.read_domains = I915_GEM_DOMAIN_GTT;
		if (data->map_flags & BO_MAP_WRITE)
			set_domain.write_domain = I915_GEM_DOMAIN_GTT;
	}

	ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_SET_DOMAIN, &set_domain);
	if (ret) {
		ALOGE("drv: DRM_IOCTL_I915_GEM_SET_DOMAIN with %d\n", ret);
		return ret;
	}

	return 0;
}

static int i915_bo_flush(struct bo *bo, struct map_info *data)
{
	struct i915_device *i915 = bo->drv->priv;
	if (!i915->has_llc && bo->tiling == I915_TILING_NONE)
		i915_clflush(data->addr, data->length);

	return 0;
}

static uint32_t i915_resolve_format(uint32_t format, uint64_t use_flags)
{
	uint32_t resolved_format;
	if (i915_private_resolve_format(format, use_flags, &resolved_format)) {
		return resolved_format;
	}

	switch (format) {
	case DRM_FORMAT_FLEX_IMPLEMENTATION_DEFINED:
		/* KBL camera subsystem requires NV12. */
		if (use_flags & (BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE))
			return DRM_FORMAT_NV12;
		/*HACK: See b/28671744 */
		return DRM_FORMAT_XBGR8888;
	case DRM_FORMAT_FLEX_YCbCr_420_888:
		/*
		 * KBL camera subsystem requires NV12. Our other use cases
		 * don't care:
		 * - Hardware video supports NV12,
		 * - USB Camera HALv3 supports NV12,
		 * - USB Camera HALv1 doesn't use this format.
		 * Moreover, NV12 is preferred for video, due to overlay
		 * support on SKL+.
		 */
		return DRM_FORMAT_NV12;
	default:
		return format;
	}
}

struct backend backend_i915 = {
	.name = "i915",
	.init = i915_init,
	.close = i915_close,
	.bo_create = i915_bo_create,
	.bo_create_with_modifiers = i915_bo_create_with_modifiers,
	.bo_destroy = drv_gem_bo_destroy,
	.bo_import = i915_bo_import,
	.bo_map = i915_bo_map,
	.bo_unmap = drv_bo_munmap,
	.bo_invalidate = i915_bo_invalidate,
	.bo_flush = i915_bo_flush,
	.resolve_format = i915_resolve_format,
};

struct backend backend_i915_bpo = {
	.name = "i915_bpo",
	.init = i915_init,
	.close = i915_close,
	.bo_create = i915_bo_create,
	.bo_create_with_modifiers = i915_bo_create_with_modifiers,
	.bo_destroy = drv_gem_bo_destroy,
	.bo_import = i915_bo_import,
	.bo_map = i915_bo_map,
	.bo_unmap = drv_bo_munmap,
	.bo_invalidate = i915_bo_invalidate,
	.bo_flush = i915_bo_flush,
	.resolve_format = i915_resolve_format,
};

#endif
