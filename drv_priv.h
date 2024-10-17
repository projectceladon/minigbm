/*
 * Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef DRV_PRIV_H
#define DRV_PRIV_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>

#include "drv.h"

struct bo_metadata {
	uint32_t width;
	uint32_t height;
	uint32_t format;
	uint32_t tiling;
	size_t num_planes;
	uint32_t offsets[DRV_MAX_PLANES];
	uint32_t sizes[DRV_MAX_PLANES];
	uint32_t strides[DRV_MAX_PLANES];
	uint64_t format_modifier;
	uint64_t use_flags;
	size_t total_size;

	/*
	 * Most of the following metadata is virtgpu cross_domain specific.  However, that backend
	 * needs to know traditional metadata (strides, offsets) in addition to this backend
	 * specific metadata.  It's easiest just to stuff all the metadata here rather than
	 * having two metadata structs.
	 */
	uint32_t blob_id;
	uint32_t map_info;
	int32_t memory_idx;
	int32_t physical_device_idx;
};

struct bo {
	struct driver *drv;
	struct bo_metadata meta;
	bool is_test_buffer;
	union bo_handle handles[DRV_MAX_PLANES];
	void *priv;
};

struct format_metadata {
	uint32_t priority;
	uint32_t tiling;
	uint64_t modifier;
};

struct combination {
	uint32_t format;
	struct format_metadata metadata;
	uint64_t use_flags;
};

enum {
	GPU_GRP_TYPE_INTEL_IGPU_IDX = 0,
	GPU_GRP_TYPE_INTEL_DGPU_IDX = 1,
	GPU_GRP_TYPE_VIRTIO_GPU_BLOB_IDX = 2,
	// virtio-GPU with allow-p2p feature, implying its display is backed by dGPU
	GPU_GRP_TYPE_VIRTIO_GPU_BLOB_P2P_IDX = 3,
	GPU_GRP_TYPE_VIRTIO_GPU_NO_BLOB_IDX = 4,
	GPU_GRP_TYPE_VIRTIO_GPU_IVSHMEM_IDX = 5,
	GPU_GRP_TYPE_NR,
};

#define GPU_GRP_TYPE_HAS_INTEL_IGPU_BIT			(1ull << GPU_GRP_TYPE_INTEL_IGPU_IDX)
#define GPU_GRP_TYPE_HAS_INTEL_DGPU_BIT			(1ull << GPU_GRP_TYPE_INTEL_DGPU_IDX)
#define GPU_GRP_TYPE_HAS_VIRTIO_GPU_BLOB_BIT		(1ull << GPU_GRP_TYPE_VIRTIO_GPU_BLOB_IDX)
#define GPU_GRP_TYPE_HAS_VIRTIO_GPU_BLOB_P2P_BIT	(1ull << GPU_GRP_TYPE_VIRTIO_GPU_BLOB_P2P_IDX)
#define GPU_GRP_TYPE_HAS_VIRTIO_GPU_NO_BLOB_BIT		(1ull << GPU_GRP_TYPE_VIRTIO_GPU_NO_BLOB_IDX)
#define GPU_GRP_TYPE_HAS_VIRTIO_GPU_IVSHMEM_BIT		(1ull << GPU_GRP_TYPE_VIRTIO_GPU_IVSHMEM_IDX)

#define DRIVER_DEVICE_FEATURE_I915_DGPU			(1ull << 1)
#define DRIVER_DEVICE_FEATURE_VIRGL_RESOURCE_BLOB	(1ull << 2)
#define DRIVER_DEVICE_FEATURE_VIRGL_QUERY_DEV		(1ull << 3)
#define DRIVER_DEVICE_FEATURE_VIRGL_ALLOW_P2P		(1ull << 4)

struct driver {
	int fd;
	const struct backend *backend;
	void *priv;
	pthread_mutex_t buffer_table_lock;
	void *buffer_table;
	uint64_t gpu_grp_type;
	pthread_mutex_t mappings_lock;
	struct drv_array *mappings;
	struct drv_array *combos;
	bool compression;
};

struct backend {
	char *name;
	void (*preload)(bool load);
	int (*init)(struct driver *drv);
	void (*close)(struct driver *drv);
	int (*bo_create)(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
			 uint64_t use_flags);
	int (*bo_create_with_modifiers)(struct bo *bo, uint32_t width, uint32_t height,
					uint32_t format, const uint64_t *modifiers, uint32_t count);
	// Either both or neither _metadata functions must be implemented.
	// If the functions are implemented, bo_create and bo_create_with_modifiers must not be.
	int (*bo_compute_metadata)(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
				   uint64_t use_flags, const uint64_t *modifiers, uint32_t count);
	int (*bo_create_from_metadata)(struct bo *bo);
	/* Called for every non-test-buffer BO on free */
	int (*bo_release)(struct bo *bo);
	/* Called on free if this bo is the last object referencing the contained GEM BOs */
	int (*bo_destroy)(struct bo *bo);
	int (*bo_import)(struct bo *bo, struct drv_import_fd_data *data);
	void *(*bo_map)(struct bo *bo, struct vma *vma, uint32_t map_flags);
	int (*bo_unmap)(struct bo *bo, struct vma *vma);
	int (*bo_invalidate)(struct bo *bo, struct mapping *mapping);
	int (*bo_flush)(struct bo *bo, struct mapping *mapping);
	void (*resolve_format_and_use_flags)(struct driver *drv, uint32_t format,
					     uint64_t use_flags, uint32_t *out_format,
					     uint64_t *out_use_flags);
	size_t (*num_planes_from_modifier)(struct driver *drv, uint32_t format, uint64_t modifier);
	int (*resource_info)(struct bo *bo, uint32_t strides[DRV_MAX_PLANES],
			     uint32_t offsets[DRV_MAX_PLANES], uint64_t *format_modifier);
	uint32_t (*get_max_texture_2d_size)(struct driver *drv);
	bool (*is_feature_supported)(struct driver *drv, uint64_t feature);
};

// clang-format off
#define BO_USE_RENDER_MASK (BO_USE_LINEAR | BO_USE_RENDERING | BO_USE_RENDERSCRIPT | \
			    BO_USE_SW_READ_OFTEN | BO_USE_SW_WRITE_OFTEN | BO_USE_SW_READ_RARELY | \
			    BO_USE_SW_WRITE_RARELY | BO_USE_TEXTURE | BO_USE_FRONT_RENDERING)

#define BO_USE_TEXTURE_MASK (BO_USE_LINEAR | BO_USE_RENDERSCRIPT | BO_USE_SW_READ_OFTEN | \
			     BO_USE_SW_WRITE_OFTEN | BO_USE_SW_READ_RARELY | \
			     BO_USE_SW_WRITE_RARELY | BO_USE_TEXTURE | BO_USE_FRONT_RENDERING)

#define BO_USE_SW_MASK (BO_USE_SW_READ_OFTEN | BO_USE_SW_WRITE_OFTEN | \
			BO_USE_SW_READ_RARELY | BO_USE_SW_WRITE_RARELY | BO_USE_FRONT_RENDERING)

#define BO_USE_GPU_HW (BO_USE_RENDERING | BO_USE_TEXTURE | BO_USE_GPU_DATA_BUFFER)

#define BO_USE_NON_GPU_HW (BO_USE_SCANOUT | BO_USE_CAMERA_WRITE | BO_USE_CAMERA_READ | \
			   BO_USE_HW_VIDEO_ENCODER | BO_USE_HW_VIDEO_DECODER | BO_USE_SENSOR_DIRECT_DATA)

#define BO_USE_HW_MASK	(BO_USE_GPU_HW | BO_USE_NON_GPU_HW)

#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR DRM_FORMAT_MOD_NONE
#endif

#define LINEAR_METADATA (struct format_metadata) { 1, 0, DRM_FORMAT_MOD_LINEAR }

#define MESA_LLVMPIPE_MAX_TEXTURE_2D_LEVELS 15
#define MESA_LLVMPIPE_MAX_TEXTURE_2D_SIZE (1 << (MESA_LLVMPIPE_MAX_TEXTURE_2D_LEVELS - 1))
#define MESA_LLVMPIPE_TILE_ORDER 6
#define MESA_LLVMPIPE_TILE_SIZE (1 << MESA_LLVMPIPE_TILE_ORDER)

// clang-format on

#endif
