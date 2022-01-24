/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef CROS_GRALLOC_DRIVER_H
#define CROS_GRALLOC_DRIVER_H

#include "cros_gralloc_buffer.h"

#include <unordered_map>
#include <vector>

#include "cros_gralloc_spinlock.h"

enum {
  GRALLOC_EVENT_ALLOCATE  = 0,
  GRALLOC_EVENT_RETAIN    = 1,
  GRALLOC_EVENT_RELEASE   = 2,
};
typedef void (*cros_gralloc_cb)(void* ctx, int event, const buffer_handle_t buffer);


class cros_gralloc_driver
{
      public:
	cros_gralloc_driver();
	~cros_gralloc_driver();

	int32_t init();  
	bool is_supported(const struct cros_gralloc_buffer_descriptor *descriptor);
	int32_t allocate(const struct cros_gralloc_buffer_descriptor *descriptor,
			 buffer_handle_t *out_handle);

	int32_t retain(buffer_handle_t handle);
	int32_t release(buffer_handle_t handle);

	int32_t lock(buffer_handle_t handle, int32_t acquire_fence, uint32_t map_flags,
		     uint8_t *addr[DRV_MAX_PLANES]);
	int32_t unlock(buffer_handle_t handle, int32_t *release_fence);

	int32_t get_backing_store(buffer_handle_t handle, uint64_t *out_store);

    int init_kms();
    int get_kms_info(kms_info_t* info);
    int kms_present(buffer_handle_t handle);
    void fini_kms();
    int32_t addCallback(cros_gralloc_cb cb, void* ctx);

      private:
	cros_gralloc_driver(cros_gralloc_driver const &);
	cros_gralloc_driver operator=(cros_gralloc_driver const &);
	cros_gralloc_buffer *get_buffer(cros_gralloc_handle_t hnd);

	struct driver *drv_;
        SpinLock mutex_;
	std::unordered_map<uint32_t, cros_gralloc_buffer *> buffers_;
	std::unordered_map<cros_gralloc_handle_t, std::pair<cros_gralloc_buffer *, int32_t>>
	    handles_;
	std::unordered_map<cros_gralloc_cb, void*> cbs_;
};

#endif
