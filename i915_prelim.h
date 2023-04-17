/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#ifndef I915_PRELIM
#define I915_PRELIM

#include <i915_drm.h>

#define PRELIM_DRM_I915_QUERY           		(1 << 16)
#define PRELIM_DRM_I915_QUERY_MEMORY_REGIONS    (PRELIM_DRM_I915_QUERY | 4)
#define PRELIM_I915_OBJECT_PARAM  				(1ull << 48)
#define PRELIM_I915_PARAM_MEMORY_REGIONS 		((1 << 16) | 0x1)
#define PRELIM_I915_USER_EXT        			(1 << 16)
#define PRELIM_I915_GEM_CREATE_EXT_SETPARAM     (PRELIM_I915_USER_EXT | 1)
#define PRELIM_DRM_IOCTL_I915_GEM_CREATE_EXT		DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_CREATE, struct prelim_drm_i915_gem_create_ext)

#define prelim_drm_i915_gem_memory_class_instance drm_i915_gem_memory_class_instance
struct prelim_drm_i915_gem_object_param {
	/* Object handle (0 for I915_GEM_CREATE_EXT_SETPARAM) */
	__u32 handle;

	/* Data pointer size */
	__u32 size;

/*
 * PRELIM_I915_OBJECT_PARAM:
 *
 * Select object namespace for the param.
 */
#define PRELIM_I915_OBJECT_PARAM  (1ull << 48)

/*
 * PRELIM_I915_PARAM_MEMORY_REGIONS:
 *
 * Set the data pointer with the desired set of placements in priority
 * order(each entry must be unique and supported by the device), as an array of
 * prelim_drm_i915_gem_memory_class_instance, or an equivalent layout of class:instance
 * pair encodings. See PRELIM_DRM_I915_QUERY_MEMORY_REGIONS for how to query the
 * supported regions.
 *
 * Note that this requires the PRELIM_I915_OBJECT_PARAM namespace:
 *	.param = PRELIM_I915_OBJECT_PARAM | PRELIM_I915_PARAM_MEMORY_REGIONS
 */
#define PRELIM_I915_PARAM_MEMORY_REGIONS ((1 << 16) | 0x1)
	__u64 param;

	/* Data value or pointer */
	__u64 data;
};

struct prelim_drm_i915_gem_create_ext_setparam {
	struct i915_user_extension base;
	struct prelim_drm_i915_gem_object_param param;
};

/**
 * struct prelim_drm_i915_memory_region_info
 *
 * Describes one region as known to the driver.
 */
struct prelim_drm_i915_memory_region_info {
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

struct prelim_drm_i915_query_memory_regions {
    /** @num_regions: Number of supported regions */
    __u32 num_regions;

    /** @rsvd: MBZ */
    __u32 rsvd[3];

    /** @regions: Info about each supported region */
    struct prelim_drm_i915_memory_region_info regions[];
};


struct prelim_drm_i915_gem_create_ext {

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
#define PRELIM_I915_GEM_CREATE_EXT_SETPARAM	(PRELIM_I915_USER_EXT | 1)
#define PRELIM_I915_GEM_CREATE_EXT_FLAGS_UNKNOWN \
	(~PRELIM_I915_GEM_CREATE_EXT_SETPARAM)
	__u64 extensions;
};
#endif
