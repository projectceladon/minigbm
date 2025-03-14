// Copyright (c) 2024 Intel Corporation
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#ifndef INTEL_DEVICE_
#define INTEL_DEVICE_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <drv_priv.h>

struct intel_gpu_info {
	int graphics_version;
	int sub_version;
	bool is_xelpd;
};

int intel_gpu_info_from_device_id(uint16_t device_id, struct intel_gpu_info *info);

bool is_intel_dg2(int fd);
bool is_virtio_gpu_with_allow_p2p(int virtgpu_fd);
bool is_virtio_gpu_with_blob(int virtgpu_fd);
bool is_virtio_gpu_pci_device(int virtgpu_fd);

int get_gpu_type(int fd);
bool is_virtio_gpu_owned_by_lic(int fd);

#ifdef __cplusplus
}
#endif

#endif
