/*
 * Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef CROS_GRALLOC_HELPERS_H
#define CROS_GRALLOC_HELPERS_H

#include "../drv.h"
#include "cros_gralloc_handle.h"

#include <aidl/android/hardware/graphics/common/BlendMode.h>
#include <aidl/android/hardware/graphics/common/Dataspace.h>
#include <log/log.h>
#include <system/graphics.h>
#include <system/window.h>

#include <string>

// Reserve the GRALLOC_USAGE_PRIVATE_0 bit from hardware/gralloc.h for buffers
// used for front rendering. minigbm backend later decides to use
// BO_USE_FRONT_RENDERING or BO_USE_LINEAR upon buffer allocaton.
#define BUFFER_USAGE_FRONT_RENDERING (1U << 28)

// Adopt BufferUsage::FRONT_BUFFER from api level 33
#define BUFFER_USAGE_FRONT_RENDERING_MASK (BUFFER_USAGE_FRONT_RENDERING | (1ULL << 32))

#define CROS_GRALLOC_BUFFER_METADATA_MAX_NAME_SIZE 1024

struct cros_gralloc_buffer_descriptor {
	uint32_t width;
	uint32_t height;
	int32_t droid_format;
	int64_t droid_usage;
	uint32_t drm_format;
	uint64_t use_flags;
	// If true, allocate an additional shared memory region for buffer metadata.
	bool enable_metadata_fd = false;
	// If the additional shared memory region for buffer metadata is present, the
	// additional amount of space reserved for client use.
	uint64_t client_metadata_size = 0;
	std::string name;
	aidl::android::hardware::graphics::common::Dataspace dataspace =
	    aidl::android::hardware::graphics::common::Dataspace::UNKNOWN;
	aidl::android::hardware::graphics::common::BlendMode blend =
	    aidl::android::hardware::graphics::common::BlendMode::INVALID;
#ifdef USE_GRALLOC1
	uint32_t consumer_usage;
	uint32_t producer_usage;
#endif
};

constexpr uint32_t cros_gralloc_magic = 0xABCDDCBA;
constexpr uint32_t handle_data_size =
    ((sizeof(struct cros_gralloc_handle) - offsetof(cros_gralloc_handle, fds[0])) / sizeof(int));

uint32_t cros_gralloc_convert_format(int32_t format);

uint64_t cros_gralloc_convert_usage(uint64_t usage);

uint32_t cros_gralloc_convert_map_usage(uint64_t usage);

cros_gralloc_handle_t cros_gralloc_convert_handle(buffer_handle_t handle);

int32_t cros_gralloc_sync_wait(int32_t fence, bool close_fence);

#ifdef USE_GRALLOC1
int32_t cros_gralloc_sync_wait(int32_t acquire_fence);
#endif

std::string get_drm_format_string(uint32_t drm_format);

#endif
