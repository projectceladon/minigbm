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

#include <drm.h>
#include <fcntl.h>
#include <i915_drm.h>
#include <log/log.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

#include "external/virtgpu_drm.h"

#include "intel_device.h"

#define ARRAY_SIZE(A) (sizeof(A) / sizeof(*(A)))

#define GEN_VERSION_X10(dev) ((dev)->graphics_version * 10 + (dev)->sub_version)

#define VIRTGPU_PARAM_QUERY_DEV		11 /* Query the virtio device name. */
#define VIRTGPU_PARAM_ALLOW_P2P		12

static int gem_param(int fd, int name)
{
	int v = -1; /* No param uses (yet) the sign bit, reserve it for errors */

	struct drm_i915_getparam gp = { .param = name, .value = &v };
	if (drmIoctl(fd, DRM_IOCTL_I915_GETPARAM, &gp))
		return -1;

	return v;
}

int intel_gpu_info_from_device_id(uint16_t device_id, struct intel_gpu_info *i915)
{
	const uint16_t gen4_ids[] = { 0x29A2, 0x2992, 0x2982, 0x2972, 0x2A02, 0x2A12, 0x2A42,
				      0x2E02, 0x2E12, 0x2E22, 0x2E32, 0x2E42, 0x2E92 };
	const uint16_t gen5_ids[] = { 0x0042, 0x0046 };
	const uint16_t gen6_ids[] = { 0x0102, 0x0112, 0x0122, 0x0106, 0x0116, 0x0126, 0x010A };
	const uint16_t gen7_ids[] = {
		0x0152, 0x0162, 0x0156, 0x0166, 0x015a, 0x016a, 0x0402, 0x0412, 0x0422,
		0x0406, 0x0416, 0x0426, 0x040A, 0x041A, 0x042A, 0x040B, 0x041B, 0x042B,
		0x040E, 0x041E, 0x042E, 0x0C02, 0x0C12, 0x0C22, 0x0C06, 0x0C16, 0x0C26,
		0x0C0A, 0x0C1A, 0x0C2A, 0x0C0B, 0x0C1B, 0x0C2B, 0x0C0E, 0x0C1E, 0x0C2E,
		0x0A02, 0x0A12, 0x0A22, 0x0A06, 0x0A16, 0x0A26, 0x0A0A, 0x0A1A, 0x0A2A,
		0x0A0B, 0x0A1B, 0x0A2B, 0x0A0E, 0x0A1E, 0x0A2E, 0x0D02, 0x0D12, 0x0D22,
		0x0D06, 0x0D16, 0x0D26, 0x0D0A, 0x0D1A, 0x0D2A, 0x0D0B, 0x0D1B, 0x0D2B,
		0x0D0E, 0x0D1E, 0x0D2E, 0x0F31, 0x0F32, 0x0F33, 0x0157, 0x0155
	};
	const uint16_t gen8_ids[] = { 0x22B0, 0x22B1, 0x22B2, 0x22B3, 0x1602, 0x1606,
				      0x160A, 0x160B, 0x160D, 0x160E, 0x1612, 0x1616,
				      0x161A, 0x161B, 0x161D, 0x161E, 0x1622, 0x1626,
				      0x162A, 0x162B, 0x162D, 0x162E };
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
	const uint16_t gen11_ids[] = { 0x8A50, 0x8A51, 0x8A52, 0x8A53, 0x8A54, 0x8A56, 0x8A57,
				       0x8A58, 0x8A59, 0x8A5A, 0x8A5B, 0x8A5C, 0x8A5D, 0x8A71,
				       0x4500, 0x4541, 0x4551, 0x4555, 0x4557, 0x4571, 0x4E51,
				       0x4E55, 0x4E57, 0x4E61, 0x4E71 };
	const uint16_t gen12_ids[] = {
		0x4c8a, 0x4c8b, 0x4c8c, 0x4c90, 0x4c9a, 0x4680, 0x4681, 0x4682, 0x4683, 0x4688,
		0x4689, 0x4690, 0x4691, 0x4692, 0x4693, 0x4698, 0x4699, 0x4626, 0x4628, 0x462a,
		0x46a0, 0x46a1, 0x46a2, 0x46a3, 0x46a6, 0x46a8, 0x46aa, 0x46b0, 0x46b1, 0x46b2,
		0x46b3, 0x46c0, 0x46c1, 0x46c2, 0x46c3, 0x9A40, 0x9A49, 0x9A59, 0x9A60, 0x9A68,
		0x9A70, 0x9A78, 0x9AC0, 0x9AC9, 0x9AD9, 0x9AF8, 0x4905, 0x4906, 0x4907, 0x4908
	};
	const uint16_t adlp_ids[] = { 0x46A0, 0x46A1, 0x46A2, 0x46A3, 0x46A6, 0x46A8, 0x46AA,
				      0x462A, 0x4626, 0x4628, 0x46B0, 0x46B1, 0x46B2, 0x46B3,
				      0x46C0, 0x46C1, 0x46C2, 0x46C3, 0x46D0, 0x46D1, 0x46D2 };

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

	const uint16_t rplp_ids[] = { 0xA720, 0xA721, 0xA7A0, 0xA7A1, 0xA7A8, 0xA7A9 };

	const uint16_t mtl_ids[] = { 0x7D40, 0x7D60, 0x7D45, 0x7D55, 0x7DD5 };

	unsigned i;
	/* Gen 4 */
	for (i = 0; i < ARRAY_SIZE(gen4_ids); i++)
		if (gen4_ids[i] == device_id) {
			i915->graphics_version = 4;
			i915->sub_version = 0;
			i915->is_xelpd = false;
			return 0;
		}

	/* Gen 5 */
	for (i = 0; i < ARRAY_SIZE(gen5_ids); i++)
		if (gen5_ids[i] == device_id) {
			i915->graphics_version = 5;
			i915->sub_version = 0;
			i915->is_xelpd = false;
			return 0;
		}

	/* Gen 6 */
	for (i = 0; i < ARRAY_SIZE(gen6_ids); i++)
		if (gen6_ids[i] == device_id) {
			i915->graphics_version = 6;
			i915->sub_version = 0;
			i915->is_xelpd = false;
			return 0;
		}

	/* Gen 7 */
	for (i = 0; i < ARRAY_SIZE(gen7_ids); i++)
		if (gen7_ids[i] == device_id) {
			i915->graphics_version = 7;
			return 0;
		}

	/* Gen 8 */
	for (i = 0; i < ARRAY_SIZE(gen8_ids); i++)
		if (gen8_ids[i] == device_id) {
			i915->graphics_version = 8;
			i915->sub_version = 0;
			i915->is_xelpd = false;
			return 0;
		}

	/* Gen 9 */
	for (i = 0; i < ARRAY_SIZE(gen9_ids); i++)
		if (gen9_ids[i] == device_id) {
			i915->graphics_version = 9;
			i915->sub_version = 0;
			i915->is_xelpd = false;
			return 0;
		}

	/* Gen 11 */
	for (i = 0; i < ARRAY_SIZE(gen11_ids); i++)
		if (gen11_ids[i] == device_id) {
			i915->graphics_version = 11;
			i915->sub_version = 0;
			i915->is_xelpd = false;
			return 0;
		}

	/* Gen 12 */
	for (i = 0; i < ARRAY_SIZE(gen12_ids); i++)
		if (gen12_ids[i] == device_id) {
			i915->graphics_version = 12;
			i915->sub_version = 0;
			i915->is_xelpd = false;
			return 0;
		}

	for (i = 0; i < ARRAY_SIZE(dg2_ids); i++)
		if (dg2_ids[i] == device_id) {
			i915->graphics_version = 12;
			i915->sub_version = 5;
			i915->is_xelpd = false;
			return 0;
		}

	for (i = 0; i < ARRAY_SIZE(adlp_ids); i++)
		if (adlp_ids[i] == device_id) {
			i915->graphics_version = 12;
			i915->sub_version = 5;
			i915->is_xelpd = true;
			return 0;
		}

	for (i = 0; i < ARRAY_SIZE(rplp_ids); i++)
		if (rplp_ids[i] == device_id) {
			i915->graphics_version = 12;
			i915->sub_version = 0;
			i915->is_xelpd = true;
			return 0;
		}

	for (i = 0; i < ARRAY_SIZE(mtl_ids); i++)
		if (mtl_ids[i] == device_id) {
			i915->graphics_version = 14;
			i915->sub_version = 0;
			i915->is_xelpd = false;
			return 0;
		}

	return -1;
}

bool is_intel_dg2(int fd)
{
	int ret;
	uint16_t device_id;
	struct intel_gpu_info info;

	ret = gem_param(fd, I915_PARAM_CHIPSET_ID);
	if (ret == -1) {
		return false;
	}
	device_id = (uint16_t)ret;
	ret = intel_gpu_info_from_device_id(device_id, &info);
	return GEN_VERSION_X10(&info) == 125;
}

bool is_virtio_gpu_with_allow_p2p(int virtgpu_fd)
{
	struct drm_virtgpu_getparam get_param = { 0, 0 };
	uint64_t value = 0;
	get_param.param = VIRTGPU_PARAM_ALLOW_P2P;
	get_param.value = (__u64)&value;
	int ret = drmIoctl(virtgpu_fd, DRM_IOCTL_VIRTGPU_GETPARAM, &get_param);
	if (ret || value != 1) {
		return false;
	}
	return true;
}

bool is_virtio_gpu_pci_device(int virtgpu_fd)
{
	struct drm_virtgpu_getparam get_param = { 0, 0 };
	uint64_t value = 0;
	get_param.param = VIRTGPU_PARAM_QUERY_DEV;
	get_param.value = (__u64)&value;
	int ret = drmIoctl(virtgpu_fd, DRM_IOCTL_VIRTGPU_GETPARAM, &get_param);
	if (ret || value != 1) {
		return false;
	}
	return true;
}

bool is_virtio_gpu_with_blob(int virtgpu_fd)
{
	struct drm_virtgpu_getparam get_param = { 0, 0 };
	uint64_t value = 0;
	get_param.param = VIRTGPU_PARAM_RESOURCE_BLOB;
	get_param.value = (__u64)&value;
	int ret = drmIoctl(virtgpu_fd, DRM_IOCTL_VIRTGPU_GETPARAM, &get_param);
	if (ret || value != 1) {
		return false;
	}
	return true;
}

int get_gpu_type(int fd)
{
	int type = -1;
	drmVersionPtr version = drmGetVersion(fd);
	if (version == NULL) {
		return type;
	}
	if (strcmp(version->name, "i915") == 0) {
		if (is_intel_dg2(fd)) {
			type = GPU_GRP_TYPE_INTEL_DGPU_IDX;
		} else {
			type = GPU_GRP_TYPE_INTEL_IGPU_IDX;
		}
	} else if (strcmp(version->name, "virtio_gpu") == 0) {
		if (!is_virtio_gpu_pci_device(fd)) {
			type = GPU_GRP_TYPE_VIRTIO_GPU_IVSHMEM_IDX;
		} else {
			if (!is_virtio_gpu_with_blob(fd)) {
				type = GPU_GRP_TYPE_VIRTIO_GPU_NO_BLOB_IDX;
			} else if (is_virtio_gpu_with_allow_p2p(fd)){
				type = GPU_GRP_TYPE_VIRTIO_GPU_BLOB_P2P_IDX;
			} else {
				type = GPU_GRP_TYPE_VIRTIO_GPU_BLOB_IDX;
			}
		}
	}
	drmFreeVersion(version);
	return type;
}

