/*
 * Copyright 2021 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef __VIRTGPU_H__
#define __VIRTGPU_H__

#include "drv_priv.h"
#include "external/virtgpu_drm.h"
#include "util.h"

#define PARAM(x)                                                                                   \
       (struct virtgpu_param)                                                                     \
       {                                                                                          \
               x, #x, 0                                                                           \
       }

struct virtgpu_param {
        uint64_t param;
        const char *name;
        uint32_t value;
};

static struct virtgpu_param params[] = {
       PARAM(VIRTGPU_PARAM_3D_FEATURES),          PARAM(VIRTGPU_PARAM_CAPSET_QUERY_FIX),
       PARAM(VIRTGPU_PARAM_RESOURCE_BLOB),        PARAM(VIRTGPU_PARAM_HOST_VISIBLE),
       PARAM(VIRTGPU_PARAM_CROSS_DEVICE),         PARAM(VIRTGPU_PARAM_CONTEXT_INIT),
       PARAM(VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs), PARAM(VIRTGPU_PARAM_CREATE_GUEST_HANDLE),
       PARAM(VIRTGPU_PARAM_RESOURCE_SYNC),        PARAM(VIRTGPU_PARAM_GUEST_VRAM),
       PARAM(VIRTGPU_PARAM_QUERY_DEV),            PARAM(VIRTGPU_PARAM_ALLOW_P2P),
};

enum virtgpu_param_id {
	param_3d,
	param_capset_fix,
	param_resource_blob,
	param_host_visible,
	param_cross_device,
	param_context_init,
	param_supported_capset_ids,
	param_create_guest_handle,
	param_resource_sync,
	param_guest_vram,
	param_max,
};

#endif
