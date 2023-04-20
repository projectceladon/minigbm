/*
 * Copyright 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <errno.h>
#include "cros_gralloc1_module.h"
#include "drv.h"

#include <stdlib.h>

#include <hardware/gralloc.h>
#include <hardware/fb.h>

#include <log/log.h>
#include <GLES2/gl2.h>


#include <inttypes.h>
#include "../i915_private_android.h"
#include "../i915_private_android_types.h"

/* _ANDROID_AS_GUEST_ */
#if TARGET_USE_GRALLOC_VHAL
#define GRALLOC_IMP_HARDWARE_MODULE_ID "gralloc_imp"
#endif

template <typename PFN, typename T> static gralloc1_function_pointer_t asFP(T function)
{
	static_assert(std::is_same<PFN, T>::value, "Incompatible function pointer");
	return reinterpret_cast<gralloc1_function_pointer_t>(function);
}

uint64_t cros_gralloc1_convert_usage(uint64_t producer_flags, uint64_t consumer_flags)
{
	uint64_t usage = BO_USE_NONE;

	if (consumer_flags & GRALLOC1_CONSUMER_USAGE_CURSOR)
		usage |= BO_USE_CURSOR;
	if (consumer_flags & GRALLOC1_CONSUMER_USAGE_CPU_READ)
		usage |= BO_USE_SW_READ_RARELY;
	if (consumer_flags & GRALLOC1_CONSUMER_USAGE_CPU_READ_OFTEN)
		usage |= BO_USE_SW_READ_OFTEN;
	if ((consumer_flags & GRALLOC1_CONSUMER_USAGE_HWCOMPOSER) ||
	    (consumer_flags & GRALLOC1_CONSUMER_USAGE_CLIENT_TARGET)) {
		/* HWC wants to use display hardware, but can defer to OpenGL. */
		usage |= BO_USE_TEXTURE;
	} else if (consumer_flags & GRALLOC1_CONSUMER_USAGE_GPU_TEXTURE) {
		usage |= BO_USE_TEXTURE;
	}
	if (consumer_flags & GRALLOC1_CONSUMER_USAGE_VIDEO_ENCODER)
		/*HACK: See b/30054495 */
		usage |= BO_USE_SW_READ_OFTEN;
	if (consumer_flags & GRALLOC1_CONSUMER_USAGE_CAMERA)
		usage |= BO_USE_CAMERA_READ;
	if (consumer_flags & GRALLOC1_CONSUMER_USAGE_RENDERSCRIPT)
		usage |= BO_USE_RENDERSCRIPT;

	if (producer_flags & GRALLOC1_PRODUCER_USAGE_CPU_READ)
		usage |= BO_USE_SW_READ_RARELY;
	if (producer_flags & GRALLOC1_PRODUCER_USAGE_CPU_READ_OFTEN)
		usage |= BO_USE_SW_READ_OFTEN;
	if (producer_flags & GRALLOC1_PRODUCER_USAGE_CPU_WRITE)
		usage |= BO_USE_SW_WRITE_RARELY;
	if (producer_flags & GRALLOC1_PRODUCER_USAGE_CPU_WRITE_OFTEN)
		usage |= BO_USE_SW_WRITE_OFTEN;
	if (producer_flags & GRALLOC1_PRODUCER_USAGE_GPU_RENDER_TARGET)
		usage |= BO_USE_RENDERING;
	if (producer_flags & GRALLOC1_PRODUCER_USAGE_VIDEO_DECODER)
		/* Video wants to use display hardware, but can defer to OpenGL. */
		usage |= BO_USE_SCANOUT | BO_USE_RENDERING;
	if (producer_flags & GRALLOC1_PRODUCER_USAGE_PROTECTED)
		usage |= BO_USE_PROTECTED;
	if (producer_flags & GRALLOC1_PRODUCER_USAGE_CAMERA)
		usage |= BO_USE_CAMERA_WRITE;

	return usage;
}

uint64_t cros_gralloc1_convert_map_usage(uint64_t producer_flags, uint64_t consumer_flags)
{
	uint64_t usage = BO_USE_NONE;

	if (consumer_flags & GRALLOC1_CONSUMER_USAGE_CPU_READ)
		usage |= BO_MAP_READ;
	if (consumer_flags & GRALLOC1_CONSUMER_USAGE_CPU_READ_OFTEN)
		usage |= BO_MAP_READ;
	if (consumer_flags & GRALLOC1_CONSUMER_USAGE_VIDEO_ENCODER)
		/*HACK: See b/30054495 */
		usage |= BO_MAP_READ;

	if (producer_flags & GRALLOC1_PRODUCER_USAGE_CPU_READ)
		usage |= BO_MAP_READ;
	if (producer_flags & GRALLOC1_PRODUCER_USAGE_CPU_READ_OFTEN)
		usage |= BO_MAP_READ;
	if (producer_flags & GRALLOC1_PRODUCER_USAGE_CPU_WRITE)
		usage |= BO_MAP_WRITE;
	if (producer_flags & GRALLOC1_PRODUCER_USAGE_CPU_WRITE_OFTEN)
		usage |= BO_MAP_WRITE;

	return usage;
}

bool IsSupportedYUVFormat(uint32_t droid_format)
{
	switch (droid_format) {
	case HAL_PIXEL_FORMAT_YCbCr_420_888:
	case HAL_PIXEL_FORMAT_YV12:
	case HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED:
		return true;
	default:
		return i915_private_supported_yuv_format(droid_format);
	}

	return false;
}

namespace android
{

/* CrosGralloc1 is a Singleton and pCrosGralloc1 holds pointer to its instance*/
static CrosGralloc1 *pCrosGralloc1 = NULL;

static uint32_t global_ref_count = 0;
static SpinLock global_lock;
static cros_gralloc_driver* global_driver = NULL;

cros_gralloc_driver* get_global_driver() {
    static SpinLock global_lock;
    if(global_driver) {
        global_ref_count++;
    }
    else {
        global_driver = new cros_gralloc_driver();
        if(global_driver) {
            if (global_driver->init() != 0) {
        		cros_gralloc_error("Failed to initialize driver.");
	        }
            else {
                global_ref_count = 1;
            }
        }
    }

    return global_driver;
}


void release_global_driver() {
    static SpinLock global_lock;

	if (global_ref_count > 0) {
		global_ref_count--;
	}

	if (global_ref_count == 0) {
		delete global_driver;
        global_driver = nullptr;
	}
}




/*
  * framebuffer_device_t  implementation
  */
static int gralloc1_fb_setSwapInterval(struct framebuffer_device_t* dev, int interval)
{
    gralloc1_fb_dev_t* _device = (gralloc1_fb_dev_t*)dev;
    int ret = 0;

#if DEBUG_GRALLOC_API
	ALOGI("%s: %d", __func__, __LINE__);
#endif

    if ((interval < dev->minSwapInterval) || (interval > dev->maxSwapInterval)) {
	        ret = -EINVAL;
    }
    return ret;
}

static int gralloc1_fb_setUpdateRect(struct framebuffer_device_t* dev, int left, int top, int width, int height)
{

    gralloc1_fb_dev_t* _device = (gralloc1_fb_dev_t*)dev;
    int ret = 0;

#if DEBUG_GRALLOC_API
    ALOGI("%s: %d", __func__, __LINE__);
#endif

    return 0;
}

static int gralloc1_fb_post(struct framebuffer_device_t* dev, buffer_handle_t buffer)
{
    gralloc1_fb_dev_t* _device = (gralloc1_fb_dev_t*)dev;
    int ret = 0;

#if DEBUG_GRALLOC_API
     ALOGI("%s : %d : (dev = %p, buffer = %p)", __func__, __LINE__, dev, (void*)buffer);
#endif

	if(!buffer)
        return -EINVAL;

    if(_device->driver) {
       ret = _device->driver->kms_present(buffer);
    }

#if DEBUG_GRALLOC_API
    ALOGI("%s : %d : returns %d", __func__, __LINE__, ret);
#endif

    return ret;
}

static int gralloc1_fb_compositionComplete(struct framebuffer_device_t* dev)
{
    gralloc1_fb_dev_t* _device = (gralloc1_fb_dev_t*)dev;
    int ret = 0;

#if DEBUG_GRALLOC_API
    ALOGI("%s: %d", __func__, __LINE__);
#endif


    // glFlush();
    glFinish();

    return ret;
}


void gralloc1_fb_dump(struct framebuffer_device_t* dev, char *buff, int buff_len)
{
#if DEBUG_GRALLOC_API
    ALOGI("%s: %d", __func__, __LINE__);
#endif

}


int gralloc1_fb_enableScreen(struct framebuffer_device_t* dev, int enable)
{
    gralloc1_fb_dev_t* _device = (gralloc1_fb_dev_t*)dev;
    int ret = 0;

#if DEBUG_GRALLOC_API
    ALOGI("%s: %d", __func__, __LINE__);
#endif

    return ret;
}


static int gralloc1_fb_close(struct hw_device_t* dev)
{
    gralloc1_fb_dev_t* _device = (gralloc1_fb_dev_t*)dev;
    int ret = 0;

#if DEBUG_GRALLOC_API
	ALOGI("%s: %d", __func__, __LINE__);
#endif

    if(_device->driver) {
        release_global_driver();
        _device->driver = nullptr;
    }

    free(_device);

    return ret;
}


static int gralloc1_fb_open(const struct hw_module_t *mod, struct framebuffer_device_t **dev)
{

    gralloc1_fb_dev_t* _device = NULL;


#if DEBUG_GRALLOC_API
	ALOGI("%s: %d, mod = %p", __func__, __LINE__, mod);
#endif

    _device = (gralloc1_fb_dev_t*)malloc(sizeof(gralloc1_fb_dev_t));
    if(_device == NULL) {
        ALOGE("%s: %d : out of memory!", __func__, __LINE__);
        return -ENOMEM;
    }

    _device->base.setSwapInterval       = gralloc1_fb_setSwapInterval;
	_device->base.setUpdateRect         = gralloc1_fb_setUpdateRect;
	_device->base.post                  = gralloc1_fb_post;
    _device->base.compositionComplete   = gralloc1_fb_compositionComplete;
    _device->base.dump                  = gralloc1_fb_dump;
    _device->base.enableScreen          = gralloc1_fb_enableScreen;
    _device->base.common.close          = gralloc1_fb_close;
    _device->base.common.module         = const_cast<hw_module_t *>(mod);

    _device->driver = get_global_driver();
    if(!_device->driver) {
        ALOGE("%s: %d : init driver failed!", __func__, __LINE__);
        free(_device);
        return -EINVAL;
    }


    if(_device->driver->init_kms()) {
        ALOGE("%s: %d : init kms failed!", __func__, __LINE__);
        free(_device);
        return -EINVAL;
    }

    kms_info_t info;
    memset(&info, 0, sizeof(kms_info_t));
    _device->driver->get_kms_info(&info);

    const_cast<uint32_t&>(_device->base.flags)     	= info.flags;
    const_cast<uint32_t&>(_device->base.width)      = info.width;
    const_cast<uint32_t&>(_device->base.height)     = info.height;
    const_cast<int&>(_device->base.stride)          = info.stride;

    const_cast<int&>(_device->base.format)         	= info.format;
    const_cast<float&>(_device->base.xdpi)          = info.xdpi;
    const_cast<float&>(_device->base.ydpi)          = info.ydpi;
    const_cast<float&>(_device->base.fps)           = info.fps;
    const_cast<int&>(_device->base.minSwapInterval)	= info.minSwapInterval;
    const_cast<int&>(_device->base.maxSwapInterval) = info.maxSwapInterval;
    const_cast<int&>(_device->base.numFramebuffers) = info.numFramebuffers;


#if DEBUG_GRALLOC_API
    ALOGI("fb.flags = 0x%x", _device->base.flags);
    ALOGI("fb.width  %d", _device->base.width);
    ALOGI("fb.height %d", _device->base.height);
    ALOGI("fb.stride %d", _device->base.height);
    ALOGI("fb.format 0x%x" , _device->base.format);
    ALOGI("fb.xdpi   %f", _device->base.xdpi);
    ALOGI("fb.ydpi   %f", _device->base.ydpi);
    ALOGI("fb.fps    %f", _device->base.fps);
#endif

    *dev = &_device->base;

    return 0;
}



CrosGralloc1::CrosGralloc1()
{
	getCapabilities = getCapabilitiesHook;
	getFunction = getFunctionHook;
	common.tag = HARDWARE_DEVICE_TAG;
	common.version = HARDWARE_MODULE_API_VERSION(1, 0);
	common.close = HookGrallocClose;
}

CrosGralloc1::~CrosGralloc1()
{
    if(driver) {
        release_global_driver();
    }
}

bool CrosGralloc1::Init()
{
    driver = get_global_driver();
    if(!driver) {
		cros_gralloc_error("Failed to initialize driver.");
		return false;
    }

    return true;
}

void CrosGralloc1::doGetCapabilities(uint32_t *outCount, int32_t *outCapabilities)
{
#if DEBUG_GRALLOC_API
    ALOGI("%s: %d ", __func__, __LINE__);
#endif

	if (outCapabilities == nullptr) {
		*outCount = 0;
	}
}

gralloc1_function_pointer_t CrosGralloc1::doGetFunction(int32_t intDescriptor)
{
#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : intDescriptor = %llu ", __func__, __LINE__, intDescriptor);
#endif

	constexpr auto lastDescriptor = static_cast<int32_t>(GRALLOC1_LAST_FUNCTION);
	if (intDescriptor < 0 || ((intDescriptor > lastDescriptor) && ((intDescriptor < 100) || (intDescriptor > GRALLOC1_LAST_CUSTOM)))) {
		ALOGE("Invalid function descriptor %d", intDescriptor);
		return nullptr;
	}

	auto descriptor = static_cast<gralloc1_function_descriptor_t>(intDescriptor);
	switch (descriptor) {
	case GRALLOC1_FUNCTION_DUMP:
		return asFP<GRALLOC1_PFN_DUMP>(dumpHook);
	case GRALLOC1_FUNCTION_CREATE_DESCRIPTOR:
		return asFP<GRALLOC1_PFN_CREATE_DESCRIPTOR>(createDescriptorHook);
	case GRALLOC1_FUNCTION_DESTROY_DESCRIPTOR:
		return asFP<GRALLOC1_PFN_DESTROY_DESCRIPTOR>(destroyDescriptorHook);
	case GRALLOC1_FUNCTION_SET_CONSUMER_USAGE:
		return asFP<GRALLOC1_PFN_SET_CONSUMER_USAGE>(setConsumerUsageHook);
	case GRALLOC1_FUNCTION_SET_DIMENSIONS:
		return asFP<GRALLOC1_PFN_SET_DIMENSIONS>(setDimensionsHook);
	case GRALLOC1_FUNCTION_SET_FORMAT:
		return asFP<GRALLOC1_PFN_SET_FORMAT>(setFormatHook);
	case GRALLOC1_FUNCTION_SET_PRODUCER_USAGE:
		return asFP<GRALLOC1_PFN_SET_PRODUCER_USAGE>(setProducerUsageHook);
	case GRALLOC1_FUNCTION_GET_BACKING_STORE:
		return asFP<GRALLOC1_PFN_GET_BACKING_STORE>(getBackingStoreHook);
	case GRALLOC1_FUNCTION_GET_CONSUMER_USAGE:
		return asFP<GRALLOC1_PFN_GET_CONSUMER_USAGE>(getConsumerUsageHook);
	case GRALLOC1_FUNCTION_GET_DIMENSIONS:
		return asFP<GRALLOC1_PFN_GET_DIMENSIONS>(getDimensionsHook);
	case GRALLOC1_FUNCTION_GET_FORMAT:
		return asFP<GRALLOC1_PFN_GET_FORMAT>(getFormatHook);
	case GRALLOC1_FUNCTION_GET_PRODUCER_USAGE:
		return asFP<GRALLOC1_PFN_GET_PRODUCER_USAGE>(getProducerUsageHook);
	case GRALLOC1_FUNCTION_GET_STRIDE:
		return asFP<GRALLOC1_PFN_GET_STRIDE>(getStrideHook);
	case GRALLOC1_FUNCTION_GET_BYTE_STRIDE:
		return asFP<GRALLOC1_PFN_GET_BYTE_STRIDE>(getByteStrideHook);
        case GRALLOC1_FUNCTION_GET_PRIME:
                return asFP<GRALLOC1_PFN_GET_PRIME>(getPrimeHook);
	case GRALLOC1_FUNCTION_ALLOCATE:
		if (driver) {
			return asFP<GRALLOC1_PFN_ALLOCATE>(allocateBuffers);
		} else {
			return nullptr;
		}
	case GRALLOC1_FUNCTION_RETAIN:
		return asFP<GRALLOC1_PFN_RETAIN>(managementHook<&CrosGralloc1::retain>);
	case GRALLOC1_FUNCTION_RELEASE:
		return asFP<GRALLOC1_PFN_RELEASE>(managementHook<&CrosGralloc1::release>);
	case GRALLOC1_FUNCTION_GET_NUM_FLEX_PLANES:
		return asFP<GRALLOC1_PFN_GET_NUM_FLEX_PLANES>(getNumFlexPlanesHook);
	case GRALLOC1_FUNCTION_LOCK:
		return asFP<GRALLOC1_PFN_LOCK>(lockHook<void *, &CrosGralloc1::lock>);
	case GRALLOC1_FUNCTION_LOCK_FLEX:
		return asFP<GRALLOC1_PFN_LOCK_FLEX>(
		    lockHook<struct android_flex_layout, &CrosGralloc1::lockFlex>);
	case GRALLOC1_FUNCTION_UNLOCK:
		return asFP<GRALLOC1_PFN_UNLOCK>(unlockHook);
	case GRALLOC1_FUNCTION_IMPORT_BUFFER:
		return asFP<GRALLOC1_PFN_IMPORT_BUFFER>(importBufferHook);
	case GRALLOC1_FUNCTION_SET_MODIFIER:
		return asFP<GRALLOC1_PFN_SET_MODIFIER>(setModifierHook);
	case GRALLOC1_FUNCTION_SET_INTERLACE:
		return asFP<GRALLOC1_PFN_SET_INTERLACE>(setInterlaceHook);
	case GRALLOC1_FUNCTION_SET_PROTECTIONINFO:
		return asFP<GRALLOC1_PFN_SET_PROTECTIONINFO>(setProtectionInfoHook);
	case GRALLOC1_FUNCTION_GET_MODIFIER:
		return asFP<GRALLOC1_PFN_GET_MODIFIER>(getModifierHook);
	case GRALLOC1_FUNCTION_GET_BUFFER_INFO:
		return asFP<GRALLOC1_PFN_GET_BUFFER_INFO>(getBufferInfoHook);
        case GRALLOC1_FUNCTION_ADD_CALLBACK:
		return asFP<GRALLOC1_PFN_ADD_CALLBACK>(addCallbackHook);
	case GRALLOC1_FUNCTION_INVALID:
		ALOGE("Invalid function descriptor");
		return nullptr;
	}

	ALOGE("Unknown function descriptor: %d", intDescriptor);
	return nullptr;
}

void CrosGralloc1::dump(uint32_t *outSize, char *outBuffer)
{
	ALOGV("dump(%u (%p), %p", outSize ? *outSize : 0, outSize, outBuffer);
}

int32_t CrosGralloc1::createDescriptor(gralloc1_buffer_descriptor_t *outDescriptor)
{

#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : outDescriptor= %p", __func__, __LINE__, outDescriptor);
#endif

	if (!outDescriptor)
		return CROS_GRALLOC_ERROR_BAD_DESCRIPTOR;

	struct cros_gralloc_buffer_descriptor *hnd = new cros_gralloc_buffer_descriptor();
	*outDescriptor = (gralloc1_buffer_descriptor_t)hnd;
	return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::destroyDescriptor(gralloc1_buffer_descriptor_t descriptor)
{
#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : (descriptorId = %llu)", __func__, __LINE__, descriptor);
#endif

	auto hnd = (struct cros_gralloc_buffer_descriptor *)descriptor;
	delete hnd;
	return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::setConsumerUsage(gralloc1_buffer_descriptor_t descriptorId, uint64_t intUsage)
{
#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : (descriptorId = %llu, intUsage = 0x%x)", __func__, __LINE__, descriptorId, intUsage);
#endif

	auto hnd = (struct cros_gralloc_buffer_descriptor *)descriptorId;
	hnd->consumer_usage = intUsage;
	return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::setProducerUsage(gralloc1_buffer_descriptor_t descriptorId, uint64_t intUsage)
{
#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : (descriptorId = %llu, intUsage = 0x%x)", __func__, __LINE__, descriptorId, intUsage);
#endif

	auto hnd = (struct cros_gralloc_buffer_descriptor *)descriptorId;
	hnd->producer_usage = intUsage;
	return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::setDimensions(gralloc1_buffer_descriptor_t descriptorId, uint32_t width,
				    uint32_t height)
{

#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : (descriptorId = %llu, width = %d, height = %d)", __func__, __LINE__, descriptorId, width, height);
#endif

	auto hnd = (struct cros_gralloc_buffer_descriptor *)descriptorId;
	hnd->width = width;
	hnd->height = height;
	return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::setFormat(gralloc1_buffer_descriptor_t descriptorId, int32_t format)
{

#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : (descriptorId = %llu, format = %x)", __func__, __LINE__, descriptorId, format);
#endif

	auto hnd = (struct cros_gralloc_buffer_descriptor *)descriptorId;
	hnd->droid_format = format;
	hnd->drm_format = cros_gralloc_convert_format(format);
	return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::setInterlace(buffer_handle_t buffer, uint32_t interlace)
{
        auto hnd = (cros_gralloc_handle*) cros_gralloc_convert_handle(buffer);
        if (!hnd) {
                return CROS_GRALLOC_ERROR_BAD_HANDLE;
        }
        hnd->is_interlaced = interlace;
        return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::setProtectionInfo(buffer_handle_t buffer, uint32_t protection_info)
{
        auto hnd = (cros_gralloc_handle*) cros_gralloc_convert_handle(buffer);
        if (!hnd) {
                return CROS_GRALLOC_ERROR_BAD_HANDLE;
        }
        hnd->is_encrypted = protection_info;
        return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::setModifier(gralloc1_buffer_descriptor_t descriptorId, uint64_t modifier)
{

#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : (descriptorId = %llu, modifier = %llu)", __func__, __LINE__, descriptorId, modifier);
#endif

	auto hnd = (struct cros_gralloc_buffer_descriptor *)descriptorId;
	hnd->modifier = modifier;
	return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::allocate(struct cros_gralloc_buffer_descriptor *descriptor,
			       buffer_handle_t *outBufferHandle)
{

#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : (descriptor = %p)", __func__, __LINE__, descriptor);
#endif

	// If this function is being called, it's because we handed out its function
	// pointer, which only occurs when mDevice has been loaded successfully and
	// we are permitted to allocate
	uint64_t usage =
	    cros_gralloc1_convert_usage(descriptor->producer_usage, descriptor->consumer_usage);
	descriptor->use_flags = usage;
	bool supported = driver->is_supported(descriptor);
	if (!supported && (descriptor->consumer_usage & GRALLOC1_CONSUMER_USAGE_HWCOMPOSER)) {
		descriptor->use_flags &= ~BO_USE_SCANOUT;
		supported = driver->is_supported(descriptor);
	}

	if (!supported) {
		cros_gralloc_error("Unsupported combination -- HAL format: %u, HAL flags: %u, "
				   "drv_format: %u, drv_flags: %llu",
				   descriptor->droid_format, usage, descriptor->drm_format,
				   static_cast<unsigned long long>(descriptor->use_flags));
		return CROS_GRALLOC_ERROR_UNSUPPORTED;
	}
	if (driver->allocate(descriptor, outBufferHandle)) {
        if(outBufferHandle) {
            ALOGI("%s: %d : allocate failed (descriptor = %p, *outBufferHandle = %p)", __func__, __LINE__, descriptor, *outBufferHandle);
        }

		return CROS_GRALLOC_ERROR_NO_RESOURCES;
    }

#if DEBUG_GRALLOC_API
    if(outBufferHandle) {
        ALOGI("%s: %d : (descriptor = %p, *outBufferHandle = %p)", __func__, __LINE__, descriptor, *outBufferHandle);
    }
#endif

	return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::allocateBuffers(gralloc1_device_t *device, uint32_t numDescriptors,
				      const gralloc1_buffer_descriptor_t *descriptors,
				      buffer_handle_t *outBuffers)
{
#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : (descriptors = %p)", __func__, __LINE__, descriptors);
#endif

	auto adapter = getAdapter(device);
	for (uint32_t i = 0; i < numDescriptors; i++) {
		auto descriptor = (struct cros_gralloc_buffer_descriptor *)descriptors[i];
		if (!descriptor) {
			return CROS_GRALLOC_ERROR_BAD_DESCRIPTOR;
		}

		buffer_handle_t bufferHandle = nullptr;
		int32_t error = adapter->allocate(descriptor, &bufferHandle);
		if (error != CROS_GRALLOC_ERROR_NONE) {
			return error;
		}

		outBuffers[i] = bufferHandle;
	}

	return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::retain(buffer_handle_t bufferHandle)
{

#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : (buffer = %p)", __func__, __LINE__, bufferHandle);
#endif

	if (driver->retain(bufferHandle))
		return CROS_GRALLOC_ERROR_BAD_HANDLE;

	return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::release(buffer_handle_t bufferHandle)
{

#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : (buffer = %p)", __func__, __LINE__, bufferHandle);
#endif

	if (driver->release(bufferHandle))
		return CROS_GRALLOC_ERROR_BAD_HANDLE;

	return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::lock(buffer_handle_t bufferHandle, gralloc1_producer_usage_t producerUsage,
			   gralloc1_consumer_usage_t consumerUsage,
			   const gralloc1_rect_t &accessRegion, void **outData,
			   int32_t acquireFence)
{
#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : (buffer = %p)", __func__, __LINE__, bufferHandle);
#endif

	uint64_t map_flags;
	uint8_t *addr[DRV_MAX_PLANES];

	auto hnd = cros_gralloc_convert_handle(bufferHandle);
	if (!hnd) {
		cros_gralloc_error("Invalid handle.");
		return CROS_GRALLOC_ERROR_BAD_HANDLE;
	}

	map_flags = cros_gralloc1_convert_map_usage(producerUsage, consumerUsage);

	if (driver->lock(bufferHandle, acquireFence, map_flags, addr))
		return CROS_GRALLOC_ERROR_BAD_HANDLE;

	*outData = addr[0];

	return CROS_GRALLOC_ERROR_NONE;
}

thread_local android_flex_plane_t ycbcrplanes[3];

int32_t update_flex_layout(struct android_ycbcr *ycbcr, struct android_flex_layout *outFlexLayout)
{
	outFlexLayout->format = FLEX_FORMAT_YCbCr;
	outFlexLayout->num_planes = 3;
	for (uint32_t i = 0; i < outFlexLayout->num_planes; i++) {
		ycbcrplanes[i].bits_per_component = 8;
		ycbcrplanes[i].bits_used = 8;
	}

	ycbcrplanes[0].top_left = static_cast<uint8_t *>(ycbcr->y);
	ycbcrplanes[0].component = FLEX_COMPONENT_Y;
	ycbcrplanes[0].h_increment = 1;
	ycbcrplanes[0].v_increment = static_cast<int32_t>(ycbcr->ystride);

	ycbcrplanes[1].top_left = static_cast<uint8_t *>(ycbcr->cb);
	ycbcrplanes[1].component = FLEX_COMPONENT_Cb;
	ycbcrplanes[1].h_increment = static_cast<int32_t>(ycbcr->chroma_step);
	ycbcrplanes[1].v_increment = static_cast<int32_t>(ycbcr->cstride);

	ycbcrplanes[2].top_left = static_cast<uint8_t *>(ycbcr->cr);
	ycbcrplanes[2].component = FLEX_COMPONENT_Cr;
	ycbcrplanes[2].h_increment = static_cast<int32_t>(ycbcr->chroma_step);
	ycbcrplanes[2].v_increment = static_cast<int32_t>(ycbcr->cstride);

	outFlexLayout->planes = ycbcrplanes;
	return 0;
}

int32_t CrosGralloc1::lockFlex(buffer_handle_t bufferHandle,
			       gralloc1_producer_usage_t producerUsage,
			       gralloc1_consumer_usage_t consumerUsage,
			       const gralloc1_rect_t &accessRegion,
			       struct android_flex_layout *outData, int32_t acquireFence)
{

#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : (buffer = %p)", __func__, __LINE__, bufferHandle);
#endif

	int32_t ret = -EINVAL;
	struct android_ycbcr ycbcrData;

	/*Check the format and support only for YUV format */
	auto hnd = cros_gralloc_convert_handle(bufferHandle);
	if (!hnd) {
		cros_gralloc_error("lockFlex: Invalid handle.");
		return CROS_GRALLOC_ERROR_BAD_HANDLE;
	}

	if (!IsSupportedYUVFormat(hnd->droid_format)) {
		cros_gralloc_error("lockFlex: Non-YUV format not compatible.");
		return CROS_GRALLOC_ERROR_BAD_HANDLE;
	}

	ret = lockYCbCr(bufferHandle, producerUsage, consumerUsage, accessRegion, &ycbcrData,
			acquireFence);

	/* convert the data in flex format*/
	update_flex_layout(&ycbcrData, outData);

	return ret;
}

int32_t CrosGralloc1::lockYCbCr(buffer_handle_t bufferHandle,
				gralloc1_producer_usage_t producerUsage,
				gralloc1_consumer_usage_t consumerUsage,
				const gralloc1_rect_t &accessRegion, struct android_ycbcr *ycbcr,
				int32_t acquireFence)
{

#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : (buffer = %p)", __func__, __LINE__, bufferHandle);
#endif

	uint64_t map_flags;
	uint8_t *addr[DRV_MAX_PLANES] = { nullptr, nullptr, nullptr, nullptr };

	auto hnd = cros_gralloc_convert_handle(bufferHandle);
	if (!hnd) {
		cros_gralloc_error("Invalid handle.");
		return CROS_GRALLOC_ERROR_BAD_HANDLE;
	}

	if (!IsSupportedYUVFormat(hnd->droid_format)) {
		cros_gralloc_error("Non-YUV format not compatible.");
		return CROS_GRALLOC_ERROR_BAD_HANDLE;
	}

	map_flags = cros_gralloc1_convert_map_usage(producerUsage, consumerUsage);
	if (driver->lock(bufferHandle, acquireFence, map_flags, addr))
		return CROS_GRALLOC_ERROR_BAD_HANDLE;

	switch (hnd->format) {
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV12_Y_TILED_INTEL:
		ycbcr->y = addr[0];
		ycbcr->cb = addr[1];
		ycbcr->cr = addr[1] + 1;
		ycbcr->ystride = hnd->strides[0];
		ycbcr->cstride = hnd->strides[1];
		ycbcr->chroma_step = 2;
		break;
	case DRM_FORMAT_YVU420:
	case DRM_FORMAT_YVU420_ANDROID:
		ycbcr->y = addr[0];
		ycbcr->cb = addr[2];
		ycbcr->cr = addr[1];
		ycbcr->ystride = hnd->strides[0];
		ycbcr->cstride = hnd->strides[1];
		ycbcr->chroma_step = 1;
		break;
	case DRM_FORMAT_P010:
		ycbcr->y = addr[0];
		ycbcr->cb = addr[1];
		ycbcr->cr = addr[1] + 2;
		ycbcr->ystride = hnd->strides[0];
		ycbcr->cstride = hnd->strides[1];
		ycbcr->chroma_step = 4;
		break;
	case DRM_FORMAT_NV21:
		ycbcr->y = addr[0];
		ycbcr->cb = addr[1] + 1;
		ycbcr->cr = addr[1];
		ycbcr->ystride = hnd->strides[0];
		ycbcr->cstride = hnd->strides[1];
		ycbcr->chroma_step = 2;
		break;
	default:
		return CROS_GRALLOC_ERROR_UNSUPPORTED;
	}

	return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::unlock(buffer_handle_t bufferHandle, int32_t *outReleaseFence)
{

#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : (buffer = %p)", __func__, __LINE__, bufferHandle);
#endif

	if (driver->unlock(bufferHandle, outReleaseFence))
		return CROS_GRALLOC_ERROR_BAD_HANDLE;

	return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::getNumFlexPlanes(buffer_handle_t buffer, uint32_t *outNumPlanes)
{

#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : (buffer = %p)", __func__, __LINE__, buffer);
#endif

	auto hnd = cros_gralloc_convert_handle(buffer);
	if (!hnd) {
		cros_gralloc_error("Invalid handle.");
		return CROS_GRALLOC_ERROR_BAD_HANDLE;
	}

	*outNumPlanes = drv_num_planes_from_format(hnd->format);
	return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::getBackingStore(buffer_handle_t buffer, gralloc1_backing_store_t *outStore)
{
#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : (buffer = %p)", __func__, __LINE__, buffer);
#endif

	auto hnd = cros_gralloc_convert_handle(buffer);
	if (!hnd) {
		cros_gralloc_error("Invalid handle.");
		return CROS_GRALLOC_ERROR_BAD_HANDLE;
	}

	if (driver->get_backing_store(buffer, outStore))
		return CROS_GRALLOC_ERROR_BAD_HANDLE;

	return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::getConsumerUsage(buffer_handle_t buffer,
				       uint64_t * /*gralloc1_consumer_usage_t*/ outUsage)
{
#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : (buffer = %p)", __func__, __LINE__, buffer);
#endif

	auto hnd = cros_gralloc_convert_handle(buffer);
	if (!hnd) {
		return CROS_GRALLOC_ERROR_BAD_HANDLE;
	}

	*outUsage = hnd->consumer_usage;
	return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::getDimensions(buffer_handle_t buffer, uint32_t *outWidth, uint32_t *outHeight)
{

#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : (buffer = %p)", __func__, __LINE__, buffer);
#endif

	auto hnd = cros_gralloc_convert_handle(buffer);
	if (!hnd) {
		return CROS_GRALLOC_ERROR_BAD_HANDLE;
	}

	*outWidth = hnd->aligned_width;
	*outHeight = hnd->aligned_height;
	return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::getFormat(buffer_handle_t buffer, int32_t *outFormat)
{
#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : (buffer = %p)", __func__, __LINE__, buffer);
#endif

	auto hnd = cros_gralloc_convert_handle(buffer);
	if (!hnd) {
		return CROS_GRALLOC_ERROR_BAD_HANDLE;
	}

	*outFormat = hnd->droid_format;
	return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::getProducerUsage(buffer_handle_t buffer,
				       uint64_t * /*gralloc1_producer_usage_t*/ outUsage)
{

#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : (buffer = %p)", __func__, __LINE__, buffer);
#endif

	auto hnd = cros_gralloc_convert_handle(buffer);
	if (!hnd) {
		return CROS_GRALLOC_ERROR_BAD_HANDLE;
	}

	*outUsage = hnd->producer_usage;
	return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::getStride(buffer_handle_t buffer, uint32_t *outStride)
{
#if DEBUG_GRALLOC_API
        ALOGI("%s: %d : (buffer = %p)", __func__, __LINE__, buffer);
#endif


	auto hnd = cros_gralloc_convert_handle(buffer);
	if (!hnd) {
		return CROS_GRALLOC_ERROR_BAD_HANDLE;
	}

	*outStride = hnd->pixel_stride;
	return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::getPrime(buffer_handle_t buffer, uint32_t *prime)
{
	auto hnd = cros_gralloc_convert_handle(buffer);
	if (!hnd) {
		return CROS_GRALLOC_ERROR_BAD_HANDLE;
	}

	*prime = hnd->fds[0];
	return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::getByteStride(buffer_handle_t buffer, uint32_t *outStride, uint32_t size)
{
    auto hnd = cros_gralloc_convert_handle(buffer);

    if (!outStride)
        return -EINVAL;

    if (!hnd) {
        return CROS_GRALLOC_ERROR_BAD_HANDLE;
    }

    if (size != drv_num_planes_from_format(hnd->format)) {
        ALOGE("Invalid array size- %d", size);
        return -EINVAL;
    }

    memcpy(outStride, hnd->strides, sizeof(*outStride) * size);
    return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::getModifier(buffer_handle_t buffer, uint32_t *outModifier, uint32_t size)
{
    auto hnd = cros_gralloc_convert_handle(buffer);

    if (!outModifier)
        return -EINVAL;

    if (!hnd) {
        return CROS_GRALLOC_ERROR_BAD_HANDLE;
    }

    if (size != drv_num_planes_from_format(hnd->format)) {
        ALOGE("Invalid array size- %d", size);
        return -EINVAL;
    }

    memcpy(outModifier, hnd->format_modifiers, sizeof(*outModifier) * size * 2);
    return CROS_GRALLOC_ERROR_NONE;
}

int32_t CrosGralloc1::importBuffer(const buffer_handle_t rawHandle, buffer_handle_t *outBuffer)
{
    if (!rawHandle) {
        *outBuffer = NULL;
        return GRALLOC1_ERROR_BAD_HANDLE;
    }
    auto error = driver->retain(rawHandle);
    if (error != GRALLOC1_ERROR_NONE) {
        *outBuffer = NULL;
        return error;
    }

    *outBuffer = rawHandle;
    return GRALLOC1_ERROR_NONE;
}

// static
int CrosGralloc1::HookGrallocClose(hw_device_t * dev)
{
    CrosGralloc1* pGralloc1 = CrosGralloc1::getAdapter((gralloc1_device_t*)dev);

#if DEBUG_GRALLOC_API
    ALOGI("%s: %d : (dev = %p)", __func__, __LINE__, dev);
#endif


    if(pGralloc1) {
        delete pGralloc1;
    }    

    return 0;
} // namespace android

int32_t CrosGralloc1::getBufferInfo(buffer_handle_t buffer, cros_gralloc_buffer_info *outInfo)
{
    auto hnd = cros_gralloc_convert_handle(buffer);

    if (!hnd || !outInfo)
        return -EINVAL;

    // resolve format for i915, for general, please try call drv_resolve_format
    switch (hnd->format) {
	    case DRM_FORMAT_NV12_Y_TILED_INTEL:
	    case DRM_FORMAT_FLEX_YCbCr_420_888:
	    case DRM_FORMAT_FLEX_IMPLEMENTATION_DEFINED:
		    outInfo->drm_fourcc = DRM_FORMAT_NV12;
                    outInfo->modifier = (((uint64_t) hnd->format_modifiers[0]) << 32) | (((uint64_t) hnd->format_modifiers[1]) & 0xffffffff);
		    break;
	    case DRM_FORMAT_YVU420_ANDROID:
		    outInfo->drm_fourcc = DRM_FORMAT_YVU420;
                    outInfo->modifier = (((uint64_t) hnd->format_modifiers[0]) << 32) | (((uint64_t) hnd->format_modifiers[1]) & 0xffffffff);
		    break;
	    default:
		    outInfo->drm_fourcc = hnd->format;
			outInfo->modifier = (((uint64_t) hnd->format_modifiers[0]) << 32) | (((uint64_t) hnd->format_modifiers[1]) & 0xffffffff);
		    break;
    }
    outInfo->num_fds = hnd->base.numFds;
    memcpy(outInfo->fds, hnd->fds, outInfo->num_fds * sizeof(int));
    memcpy(outInfo->offset, hnd->offsets, outInfo->num_fds * sizeof(uint32_t));
    memcpy(outInfo->stride, hnd->strides, outInfo->num_fds * sizeof(uint32_t));

    return CROS_GRALLOC_ERROR_NONE;
}

// static
int CrosGralloc1::HookDevOpen(const struct hw_module_t *mod, const char *name,
			      struct hw_device_t **device)
{

#if DEBUG_GRALLOC_API
	ALOGI("%s: %d : mod = %p, name = %s)", __func__, __LINE__, mod, name);
#endif

    int ret = 0;

    if (strcmp(name, GRALLOC_HARDWARE_MODULE_ID) == 0) {
        CrosGralloc1* pGralloc1 = new CrosGralloc1();
        if(pGralloc1) {
            if(pGralloc1->Init()) {
                pGralloc1->common.module = const_cast<hw_module_t *>(mod);
                *device = &pGralloc1->common;
                ret = 0;
            }
            else {
                ALOGE("Failed to initialize CrosGralloc1. \n");
                ret = -EINVAL;
            }
        }
        else {
    		ALOGE("Failed to allocate CrosGralloc1");
    		ret = -ENOMEM;
        }

    }
    else if (strcmp(name, GRALLOC_HARDWARE_FB0) == 0) {
        ret = gralloc1_fb_open(mod, (struct framebuffer_device_t **)device);
    }
    else {
        ALOGE("Invalid module name- %s", name);
        ret = -EINVAL;
    }

    return ret;
}

};

static struct hw_module_methods_t cros_gralloc_module_methods = {
	.open = android::CrosGralloc1::HookDevOpen,
};

hw_module_t HAL_MODULE_INFO_SYM = {
	.tag = HARDWARE_MODULE_TAG,
	.module_api_version = HARDWARE_MODULE_API_VERSION(1, 0),
#if TARGET_USE_GRALLOC_VHAL
	.id = GRALLOC_IMP_HARDWARE_MODULE_ID,
#else
	.id = GRALLOC_HARDWARE_MODULE_ID,
#endif
	.name = "Gralloc module",
	.author = "Chrome OS",
	.methods = &cros_gralloc_module_methods,
};


