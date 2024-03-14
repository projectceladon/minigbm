/*
 * Copyright 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifdef DRV_I915

#include <assert.h>
#include <cpuid.h>
#include <errno.h>
#include <i915_drm.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <cutils/properties.h>

#include "drv_priv.h"
#include "helpers.h"
#include "i915_prelim.h"
#include "util.h"

#ifdef USE_GRALLOC1
#include "i915_private.h"
#endif

#define I915_CACHELINE_SIZE 64
#define I915_CACHELINE_MASK (I915_CACHELINE_SIZE - 1)

static bool is_prelim_kernel = false;

static const uint32_t scanout_render_formats[] = { DRM_FORMAT_ABGR2101010, DRM_FORMAT_ABGR8888,
						   DRM_FORMAT_ARGB2101010, DRM_FORMAT_ARGB8888,
						   DRM_FORMAT_RGB565,	   DRM_FORMAT_XBGR2101010,
						   DRM_FORMAT_XBGR8888,	   DRM_FORMAT_XRGB2101010,
						   DRM_FORMAT_XRGB8888 };

static const uint32_t render_formats[] = { DRM_FORMAT_ABGR16161616F };

static const uint32_t texture_only_formats[] = { DRM_FORMAT_R8, DRM_FORMAT_NV12, DRM_FORMAT_P010,
#ifdef USE_GRALLOC1
						 DRM_FORMAT_YVU420, DRM_FORMAT_YVU420_ANDROID,
						 DRM_FORMAT_YUYV };
#else
						 DRM_FORMAT_YVU420, DRM_FORMAT_YVU420_ANDROID };
#endif

struct iris_memregion {
	struct drm_i915_gem_memory_class_instance region;
	uint64_t size;
};

struct i915_device {
	uint32_t genx10;
	int32_t has_llc;
#ifdef USE_GRALLOC1
	uint64_t cursor_width;
	uint64_t cursor_height;
#endif
	int32_t has_mmap_offset;
	bool has_local_mem;
	bool force_mem_local;
	bool has_fence_reg;
	struct iris_memregion vram, sys;
	int device_id;
	bool is_adlp;
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

static void i915_info_from_device_id(struct i915_device *i915)
{
	const uint16_t gen9_ids[] = {
			0x1902, 0x1906, 0x190A, 0x190B, 0x190E, 0x1912, 0x1913, 0x1915, 0x1916, 0x1917,
			0x191A, 0x191B, 0x191D, 0x191E, 0x1921, 0x1923, 0x1926, 0x1927, 0x192A, 0x192B,
			0x192D, 0x1932, 0x193A, 0x193B, 0x193D, 0x0A84, 0x1A84, 0x1A85, 0x5A84, 0x5A85,
			0x3184, 0x3185, 0x5902, 0x5906, 0x590A, 0x5908, 0x590B, 0x590E, 0x5913, 0x5915,
			0x5917, 0x5912, 0x5916, 0x591A, 0x591B, 0x591D, 0x591E, 0x5921, 0x5923, 0x5926,
			0x5927, 0x593B, 0x591C, 0x87C0, 0x87CA, 0x3E90, 0x3E93, 0x3E99, 0x3E9C, 0x3E91,
			0x3E92, 0x3E96, 0x3E98, 0x3E9A, 0x3E9B, 0x3E94, 0x3EA9, 0x3EA5, 0x3EA6, 0x3EA7,
			0x3EA8, 0x3EA1, 0x3EA4, 0x3EA0, 0x3EA3, 0x3EA2, 0x9B21, 0x9BA0, 0x9BA2, 0x9BA4,
			0x9BA5, 0x9BA8, 0x9BAA, 0x9BAB, 0x9BAC, 0x9B41, 0x9BC0, 0x9BC2, 0x9BC4, 0x9BC5,
			0x9BC6, 0x9BC8, 0x9BCA, 0x9BCB, 0x9BCC, 0x9BE6, 0x9BF6
	};
	const uint16_t gen12_ids[] = {
			0x4c8a, 0x4c8b, 0x4c8c, 0x4c90, 0x4c9a, 0x4680, 0x4681, 0x4682, 0x4683, 0x4688,
			0x4689, 0x4690, 0x4691, 0x4692, 0x4693, 0x4698, 0x4699, 0x4626, 0x4628, 0x462a,
			0x46a0, 0x46a1, 0x46a2, 0x46a3, 0x46a6, 0x46a8, 0x46aa, 0x46b0, 0x46b1, 0x46b2,
			0x46b3, 0x46c0, 0x46c1, 0x46c2, 0x46c3, 0x9A40, 0x9A49, 0x9A59, 0x9A60, 0x9A68,
			0x9A70, 0x9A78, 0x9AC0, 0x9AC9, 0x9AD9, 0x9AF8, 0x4905, 0x4906, 0x4907, 0x4908
	};
	const uint16_t adlp_ids[] = {
			0x46A0, 0x46A1, 0x46A2, 0x46A3, 0x46A6, 0x46A8,
			0x46AA, 0x462A, 0x4626, 0x4628, 0x46B0, 0x46B1,
			0x46B2, 0x46B3, 0x46C0, 0x46C1, 0x46C2, 0x46C3,
			0x46D0, 0x46D1, 0x46D2
	};
	const uint16_t dg2_ids[] = { // DG2 Val-Only Super-SKU: 4F80 - 4F87
			0x4F80, 0x4F81, 0x4F82, 0x4F83, 0x4F84, 0x4F85, 0x4F86, 0x4F87,

			// DG2 Desktop Reserved:  56A0 to 56AF
			0x56A0, 0x56A1, 0x56A2, 0x56A3, 0x56A4, 0x56A5, 0x56A6, 0x56A7,
			0x56A8, 0x56A9, 0x56AA, 0x56AB, 0x56AC, 0x56AD, 0x56AE, 0x56AF,

			// DG2 Notebook Reserved:  5690 to 569F
			0x5690, 0x5691, 0x5692, 0x5693, 0x5694, 0x5695, 0x5696, 0x5697,
			0x5698, 0x5699, 0x569A, 0x569B, 0x569C, 0x569D, 0x569E, 0x569F,

			// Workstation Reserved:  56B0 to 56BF
			0x56B0, 0x56B1, 0x56B2, 0x56B3, 0x56B4, 0x56B5, 0x56B6, 0x56B7,
			0x56B8, 0x56B9, 0x56BA, 0x56BB, 0x56BC, 0x56BD, 0x56BE, 0x56BF,

			// Server Reserved:  56C0 to 56CF
			0x56C0, 0x56C1, 0x56C2, 0x56C3, 0x56C4, 0x56C5, 0x56C6, 0x56C7,
			0x56C8, 0x56C9, 0x56CA, 0x56CB, 0x56CC, 0x56CD, 0x56CE, 0x56CF
	};

	unsigned i;
	i915->genx10 = 120;
	i915->is_adlp = false;

	for (i = 0; i < ARRAY_SIZE(gen9_ids); i++)
		if (gen9_ids[i] == i915->device_id) {
			i915->genx10 = 90;
			return;
		}

	for (i = 0; i < ARRAY_SIZE(adlp_ids); i++)
		if (adlp_ids[i] == i915->device_id) {
			i915->genx10 = 120;
			i915->is_adlp = true;
			return;
		}

	for (i = 0; i < ARRAY_SIZE(gen12_ids); i++)
		if (gen12_ids[i] == i915->device_id) {
			i915->genx10 = 120;
			return;
		}

	for (i = 0; i < ARRAY_SIZE(dg2_ids); i++)
		if (dg2_ids[i] == i915->device_id) {
			i915->genx10 = 125;
			return;
		}
	return;
}

bool i915_has_tile4(struct driver *drv)
{
	struct i915_device *i915 = drv->priv;
	return i915->genx10 >= 125;
}

bool i915_should_use_linear(struct i915_device *i915)
{
	switch (i915->device_id) {
	case 0x46A6:
		return true;
	default:
		return false;
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
	struct format_metadata metadata;
	uint64_t render, scanout_and_render, texture_only;
	bool is_kvm = vm_type() & HYPERTYPE_TYPE_KVM;

	scanout_and_render = BO_USE_RENDER_MASK | BO_USE_SCANOUT;
#ifdef USE_GRALLOC1
	render = BO_USE_RENDER_MASK & ~(BO_USE_RENDERING | BO_USE_TEXTURE);
#else
	render = BO_USE_RENDER_MASK;
#endif
	texture_only = BO_USE_TEXTURE_MASK;
	uint64_t linear_mask = BO_USE_RENDERSCRIPT | BO_USE_LINEAR | BO_USE_PROTECTED |
			       BO_USE_SW_READ_OFTEN | BO_USE_SW_WRITE_OFTEN;

	uint64_t camera_mask = BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE;

	metadata.tiling = I915_TILING_NONE;
	metadata.priority = 1;
	metadata.modifier = DRM_FORMAT_MOD_LINEAR;

	drv_add_combinations(drv, scanout_render_formats, ARRAY_SIZE(scanout_render_formats),
			     &metadata, scanout_and_render);

	drv_add_combinations(drv, render_formats, ARRAY_SIZE(render_formats), &metadata, render);

	drv_add_combinations(drv, texture_only_formats, ARRAY_SIZE(texture_only_formats), &metadata,
			     texture_only);

	drv_modify_linear_combinations(drv);
	/*
	 * Chrome uses DMA-buf mmap to write to YV12 buffers, which are then accessed by the
	 * Video Encoder Accelerator (VEA). It could also support NV12 potentially in the future.
	 */
	drv_modify_combination(drv, DRM_FORMAT_YVU420, &metadata, BO_USE_HW_VIDEO_ENCODER);
	/* IPU3 camera ISP supports only NV12 output. */
	drv_modify_combination(drv, DRM_FORMAT_NV12, &metadata,
			       BO_USE_HW_VIDEO_ENCODER | BO_USE_HW_VIDEO_DECODER |
				   BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE | BO_USE_SCANOUT);

	/* Android CTS tests require this. */
	drv_add_combination(drv, DRM_FORMAT_BGR888, &metadata, BO_USE_SW_MASK);
#ifdef USE_GRALLOC1
	drv_modify_combination(drv, DRM_FORMAT_ABGR2101010, &metadata, BO_USE_SW_MASK);
	drv_add_combination(drv, DRM_FORMAT_RGB888, &metadata, BO_USE_SW_MASK);
#endif

	/*
	 * R8 format is used for Android's HAL_PIXEL_FORMAT_BLOB and is used for JPEG snapshots
	 * from camera.
	 */
	drv_modify_combination(drv, DRM_FORMAT_R8, &metadata,
			       BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE);

	render = unset_flags(render, linear_mask | camera_mask);
	scanout_and_render = unset_flags(scanout_and_render, linear_mask |camera_mask);

	/* TODO(OAM-115771):
	* Some devices virtio has VIRTIO_GPU_F_MODIFIER feature,
	* but some devices have not. If device virtio has not
	* VIRTIO_GPU_F_MODIFIER, android display black screen with
	* qemu + SRIOV. So far, we have not found out why device
	* id of the two ADL device are both 0x46A6, one has
	* VIRTIO_GPU_F_MODIFIER feature, the other one has not.
	*
	* For now, we use LINEAR as WA for the above devices with
	* same device id.. */
	if (is_kvm && i915_should_use_linear(i915)) {
		scanout_and_render = unset_flags(scanout_and_render, BO_USE_SCANOUT);
	}

	/* On dGPU, only use linear */
	if (i915->genx10 >= 125)
		scanout_and_render = unset_flags(scanout_and_render, BO_USE_SCANOUT);

	metadata.tiling = I915_TILING_X;
	metadata.priority = 2;
	metadata.modifier = I915_FORMAT_MOD_X_TILED;

	drv_add_combinations(drv, render_formats, ARRAY_SIZE(render_formats), &metadata, render);
	drv_add_combinations(drv, scanout_render_formats, ARRAY_SIZE(scanout_render_formats),
			     &metadata, scanout_and_render);

	scanout_and_render =
	    unset_flags(scanout_and_render, BO_USE_SW_READ_RARELY | BO_USE_SW_WRITE_RARELY);

	if (i915_has_tile4(drv)) {
		metadata.tiling = I915_TILING_4;
		metadata.priority = 3;
		metadata.modifier = I915_FORMAT_MOD_4_TILED;
	} else {
		metadata.tiling = I915_TILING_Y;
		metadata.priority = 3;
		metadata.modifier = I915_FORMAT_MOD_Y_TILED;
		// dGPU do not support Tiling Y mode
		if ((drv->gpu_grp_type == TWO_GPU_IGPU_DGPU) ||
		    (drv->gpu_grp_type == THREE_GPU_IGPU_VIRTIO_DGPU)) {
			scanout_and_render = unset_flags(scanout_and_render, BO_USE_SCANOUT);
		}
	}

/* Support y-tiled NV12 and P010 for libva */
#ifdef I915_SCANOUT_Y_TILED
	drv_add_combination(drv, DRM_FORMAT_NV12, &metadata,
			    BO_USE_TEXTURE | BO_USE_HW_VIDEO_DECODER | BO_USE_SCANOUT);
#else
	drv_add_combination(drv, DRM_FORMAT_NV12, &metadata,
			    BO_USE_TEXTURE | BO_USE_HW_VIDEO_DECODER);
#endif
	drv_add_combination(drv, DRM_FORMAT_P010, &metadata,
			    BO_USE_TEXTURE | BO_USE_HW_VIDEO_DECODER);

	drv_add_combinations(drv, render_formats, ARRAY_SIZE(render_formats), &metadata, render);
	drv_add_combinations(drv, scanout_render_formats, ARRAY_SIZE(scanout_render_formats),
			     &metadata, scanout_and_render);
#ifdef USE_GRALLOC1
	i915_private_add_combinations(drv);
#endif
	return 0;
}

static int i915_align_dimensions(struct bo *bo, uint32_t tiling, uint32_t *stride,
				 uint32_t *aligned_height)
{
	uint32_t horizontal_alignment = 64;
	uint32_t vertical_alignment = 4;
	struct i915_device *i915 = bo->drv->priv;
	if (i915->genx10 >= 125) {
		horizontal_alignment = 4;
		vertical_alignment = 4;
	}

	switch (tiling) {
	default:
	case I915_TILING_NONE:
		/*
		 * The Intel GPU doesn't need any alignment in linear mode,
		 * but libva requires the allocation stride to be aligned to
		 * 16 bytes and height to 4 rows. Further, we round up the
		 * horizontal alignment so that row start on a cache line (64
		 * bytes).
		 */
		horizontal_alignment = 64;
		vertical_alignment = 4;
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

#ifdef USE_GRALLOC1
	if(DRM_FORMAT_R8 != bo->meta.format)
#endif
	*stride = ALIGN(*stride, horizontal_alignment);

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

	struct drm_i915_getparam gp = {.param = name, .value = &v };
	if (gen_ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp))
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
		drv_log("drv: Failed to query PRELIM_DRM_I915_QUERY_MEMORY_REGIONS\n");
		return false;
	}
	if (item.length <= 0) {
		return false;
	}
	struct prelim_drm_i915_query_memory_regions *meminfo = calloc(1, item.length);
	if (!meminfo) {
		 drv_log("drv: %s Exit due to memory allocation failure\n", __func__);
		return false;
	}
	item.data_ptr = (uintptr_t)meminfo;
	ret = drmIoctl(drv->fd, DRM_IOCTL_I915_QUERY, &query);
	if (ret < 0 || item.length <= 0) {
		free(meminfo);
		drv_log("%s:%d DRM_IOCTL_I915_QUERY error\n", __FUNCTION__, __LINE__);
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
		drv_log("drv: Failed to DRM_IOCTL_I915_QUERY\n");
		return false;
	}
	struct drm_i915_query_memory_regions *meminfo = calloc(1, item.length);
	if (!meminfo) {
		drv_log("drv: %s Exit due to memory allocation failure\n", __func__);
		return false;
	}
	item.data_ptr = (uintptr_t)meminfo;
	if (drmIoctl(drv->fd, DRM_IOCTL_I915_QUERY, &query) || item.length <= 0) {
		free(meminfo);
		drv_log("%s:%d DRM_IOCTL_I915_QUERY error\n", __FUNCTION__, __LINE__);
		return false;
	}
	i915_bo_update_meminfo(i915_dev, meminfo);

	free(meminfo);

	return true;
}

static int i915_init(struct driver *drv)
{
	int ret;
	struct i915_device *i915;
	drm_i915_getparam_t get_param;

	i915 = calloc(1, sizeof(*i915));
	if (!i915)
		return -ENOMEM;

	memset(&get_param, 0, sizeof(get_param));
	get_param.param = I915_PARAM_CHIPSET_ID;
	get_param.value = &i915->device_id;
	ret = drmIoctl(drv->fd, DRM_IOCTL_I915_GETPARAM, &get_param);
	if (ret) {
		drv_log("Failed to get I915_PARAM_CHIPSET_ID\n");
		free(i915);
		return -EINVAL;
	}

	/* must call before i915->gen is used anywhere else */
	i915_info_from_device_id(i915);

	memset(&get_param, 0, sizeof(get_param));
	get_param.param = I915_PARAM_HAS_LLC;
	get_param.value = &i915->has_llc;
	ret = drmIoctl(drv->fd, DRM_IOCTL_I915_GETPARAM, &get_param);
	if (ret) {
		drv_log("Failed to get I915_PARAM_HAS_LLC\n");
		free(i915);
		return -EINVAL;
	}

	i915->has_mmap_offset = gem_param(drv->fd, I915_PARAM_MMAP_GTT_VERSION) >= 4;
	i915->has_fence_reg = gem_param(drv->fd, I915_PARAM_NUM_FENCES_AVAIL) > 0;

	if (!i915_bo_query_prelim_meminfo(drv, i915)) {
		i915_bo_query_meminfo(drv, i915);
	} else {
		drv_info("drv: kernel supports prelim\n");
	}
#define FORCE_MEM_PROP "sys.icr.gralloc.force_mem"
	char prop[PROPERTY_VALUE_MAX];
	i915->force_mem_local = (i915->vram.size > 0) &&
				property_get(FORCE_MEM_PROP, prop, "local") > 0 &&
				!strcmp(prop, "local");
	if (i915->force_mem_local) {
		drv_info("Force to use local memory");
	}

	drv->priv = i915;

#ifdef USE_GRALLOC1
	i915_private_init(drv, &i915->cursor_width, &i915->cursor_height);
#endif

	return i915_add_combinations(drv);
}

static int i915_bo_from_format(struct bo *bo, uint32_t width, uint32_t height, uint32_t format)
{
	uint32_t offset;
	size_t plane;
	int ret, pagesize;

	offset = 0;
	pagesize = getpagesize();
	for (plane = 0; plane < drv_num_planes_from_format(format); plane++) {
		uint32_t stride = drv_stride_from_format(format, width, plane);
		uint32_t plane_height = drv_height_from_format(format, height, plane);

		if (bo->meta.tiling != I915_TILING_NONE)
			assert(IS_ALIGNED(offset, pagesize));

		ret = i915_align_dimensions(bo, bo->meta.tiling, &stride, &plane_height);
		if (ret)
			return ret;

		bo->meta.strides[plane] = stride;
		bo->meta.sizes[plane] = stride * plane_height;
		bo->meta.offsets[plane] = offset;
		offset += bo->meta.sizes[plane];
	}

	bo->meta.total_size = ALIGN(offset, pagesize);

	return 0;
}

static int i915_bo_compute_metadata(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
				    uint64_t use_flags, const uint64_t *modifiers, uint32_t count)
{
	static const uint64_t modifier_order[] = {
		I915_FORMAT_MOD_Y_TILED,
		I915_FORMAT_MOD_X_TILED,
		DRM_FORMAT_MOD_LINEAR,
	};
	uint64_t modifier;

	if (modifiers) {
		modifier =
		    drv_pick_modifier(modifiers, count, modifier_order, ARRAY_SIZE(modifier_order));
	} else {
		struct combination *combo = drv_get_combination(bo->drv, format, use_flags);
		if (!combo)
			return -EINVAL;
		modifier = combo->metadata.modifier;
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
#ifdef USE_GRALLOC1
	case I915_FORMAT_MOD_Yf_TILED:
	case I915_FORMAT_MOD_Yf_TILED_CCS:
#endif
		bo->meta.tiling = I915_TILING_Y;
		break;
	case I915_FORMAT_MOD_4_TILED:
		bo->meta.tiling = I915_TILING_4;
		break;
	}

	bo->meta.format_modifiers[0] = modifier;

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
		drv_bo_from_format(bo, stride, height, format);
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

		bo->meta.num_planes = 2;
		bo->meta.total_size = offset;
	} else {
		i915_bo_from_format(bo, width, height, format);
	}
	return 0;
}

static bool is_need_local(int64_t use_flags)
{
	static bool local = true;

	if (use_flags & BO_USE_SW_READ_RARELY || use_flags & BO_USE_SW_READ_OFTEN ||
	    use_flags & BO_USE_SW_WRITE_RARELY || use_flags & BO_USE_SW_WRITE_OFTEN) {
		local = false;
	} else {
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
	struct drm_i915_gem_set_tiling gem_set_tiling;
	struct i915_device *i915_dev = (struct i915_device *)bo->drv->priv;
	int64_t use_flags = bo->meta.use_flags;
	bool local = is_need_local(use_flags);
	if (local && i915_dev->has_local_mem) {
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
			enum iris_heap heap = flags_to_heap(i915_dev, use_flags);
			switch (heap) {
				case IRIS_HEAP_DEVICE_LOCAL_PREFERRED:
					/* For vram allocations, still use system memory as a fallback. */
					regions[ext_regions.num_regions++] = i915_dev->vram.region;
					regions[ext_regions.num_regions++] = i915_dev->sys.region;
					break;
				case IRIS_HEAP_DEVICE_LOCAL:
					regions[ext_regions.num_regions++] = i915_dev->vram.region;
					break;
				case IRIS_HEAP_SYSTEM_MEMORY:
					regions[ext_regions.num_regions++] = i915_dev->sys.region;
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
				drv_log("drv: DRM_IOCTL_I915_GEM_CREATE_EXT failed (size=%llu)\n",
					gem_create_ext.size);
				return -errno;
			} else {
				drv_info("drv: DRM_IOCTL_I915_GEM_CREATE_EXT OK (size=%llu)\n",
					gem_create_ext.size);
			}
			gem_handle = gem_create_ext.handle;

		} else {
			struct prelim_drm_i915_gem_memory_class_instance regions[2];
			uint32_t nregions = 0;
			if (i915_dev->force_mem_local) {
				/* For vram allocations, still use system memory as a fallback. */
				regions[nregions++] = i915_dev->vram.region;
				regions[nregions++] = i915_dev->sys.region;
			} else {
				regions[nregions++] = i915_dev->sys.region;
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
				drv_log("drv: PRELIM_DRM_IOCTL_I915_GEM_CREATE_EXT failed (size=%llu)\n",
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
			drv_log("DRM_IOCTL_I915_GEM_CREATE failed (size=%llu)\n", gem_create.size);
			return -errno;
		}
		gem_handle = gem_create.handle;
	}

	for (plane = 0; plane < bo->meta.num_planes; plane++)
		bo->handles[plane].u32 = gem_handle;

	if (i915_dev->genx10 != 125) {
		memset(&gem_set_tiling, 0, sizeof(gem_set_tiling));
		gem_set_tiling.handle = bo->handles[0].u32;
		gem_set_tiling.tiling_mode = bo->meta.tiling;
		gem_set_tiling.stride = bo->meta.strides[0];

		ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_SET_TILING, &gem_set_tiling);
		if (ret) {
			struct drm_gem_close gem_close;
			memset(&gem_close, 0, sizeof(gem_close));
			gem_close.handle = bo->handles[0].u32;
			drmIoctl(bo->drv->fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
			drv_log("drv: DRM_IOCTL_I915_GEM_SET_TILING failed with %d\n", errno);
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
	struct drm_i915_gem_get_tiling gem_get_tiling;
	struct i915_device *i915_dev = (struct i915_device *)(bo->drv->priv);

	ret = drv_prime_bo_import(bo, data);
	if (ret)
		return ret;

	if (i915_dev->genx10 != 125) {
		/* TODO(gsingh): export modifiers and get rid of backdoor tiling. */
		memset(&gem_get_tiling, 0, sizeof(gem_get_tiling));
		gem_get_tiling.handle = bo->handles[0].u32;

		ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_GET_TILING, &gem_get_tiling);
		if (ret) {
			drv_gem_bo_destroy(bo);
			drv_log("DRM_IOCTL_I915_GEM_GET_TILING failed.\n");
			return ret;
		}
		bo->meta.tiling = gem_get_tiling.tiling_mode;
	} else {
		bo->meta.tiling = data->tiling;
	}
	return 0;
}

static void *i915_bo_map(struct bo *bo, struct vma *vma, size_t plane, uint32_t map_flags)
{
	int ret;
	void *addr;
	struct i915_device *i915 = (struct i915_device *)(bo->drv->priv);

	if (bo->meta.format_modifiers[0] == I915_FORMAT_MOD_Y_TILED_CCS)
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
		int ret = gen_ioctl(bo->drv->fd, DRM_IOCTL_I915_GEM_MMAP_OFFSET, &mmap_arg);
		if (ret != 0 && mmap_arg.flags == I915_MMAP_OFFSET_FIXED) {
			if ((bo->meta.use_flags & BO_USE_SCANOUT) &&
			    !(bo->meta.use_flags &
			      (BO_USE_RENDERSCRIPT | BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE | BO_USE_SW_READ_OFTEN))) {
				mmap_arg.flags = I915_MMAP_OFFSET_WC;
			} else {
				mmap_arg.flags = I915_MMAP_OFFSET_WB;
			}

			ret = gen_ioctl(bo->drv->fd, DRM_IOCTL_I915_GEM_MMAP_OFFSET, &mmap_arg);
		}

		if (ret != 0) {
			drv_log("drv: DRM_IOCTL_I915_GEM_MMAP_OFFSET failed ret=%d, errno=0x%x\n",
				ret, errno);
			return MAP_FAILED;
		}

		drv_info("%s : %d : handle = %x, size = %zd, mmpa_arg.offset = %llx", __func__,
			__LINE__, mmap_arg.handle, bo->meta.total_size, mmap_arg.offset);

		/* And map it */
		addr = mmap(0, bo->meta.total_size, PROT_READ | PROT_WRITE, MAP_SHARED, bo->drv->fd,
			    mmap_arg.offset);
	} else if (bo->meta.tiling == I915_TILING_NONE) {
		struct drm_i915_gem_mmap gem_map;
		memset(&gem_map, 0, sizeof(gem_map));

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
		if (ret) {
			drv_log("DRM_IOCTL_I915_GEM_MMAP failed\n");
			return MAP_FAILED;
		}

		addr = (void *)(uintptr_t)gem_map.addr_ptr;
	} else {
		struct drm_i915_gem_mmap_gtt gem_map;
		memset(&gem_map, 0, sizeof(gem_map));

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
			        drv_log("DRM_IOCTL_I915_GEM_MMAP failed\n");
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
		drv_log("i915 GEM mmap failed\n");
		return addr;
	}

	vma->length = bo->meta.total_size;
	return addr;
}

static int i915_bo_invalidate(struct bo *bo, struct mapping *mapping)
{
	int ret;
	struct drm_i915_gem_set_domain set_domain;
	struct i915_device *i915_dev = (struct i915_device *)bo->drv->priv;

	if (i915_dev->genx10 != 125) {
		memset(&set_domain, 0, sizeof(set_domain));
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
			drv_log("DRM_IOCTL_I915_GEM_SET_DOMAIN with %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static int i915_bo_flush(struct bo *bo, struct mapping *mapping)
{
	struct i915_device *i915 = bo->drv->priv;
	if (!i915->has_llc && bo->meta.tiling == I915_TILING_NONE)
		i915_clflush(mapping->vma->addr, mapping->vma->length);

	return 0;
}

static uint32_t i915_resolve_format(struct driver *drv, uint32_t format, uint64_t use_flags)
{
#ifdef USE_GRALLOC1
	uint32_t resolved_format;
	if (i915_private_resolve_format(format, use_flags, &resolved_format)) {
		return resolved_format;
	}
#endif
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
	.resolve_format = i915_resolve_format,
};

#endif
