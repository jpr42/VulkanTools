/*
 * Vulkan
 *
 * Copyright (C) 2016-2022 Valve Corporation
 * Copyright (C) 2016-2022 LunarG, Inc.
 * Copyright (C) 2016-2022 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: Chris Forbes <chrisforbes@google.com>
 * Author: Tony Barbour <tony@lunarg.com>
 */
#include "vk_layer_table.h"
#include <vulkan/layer/vk_layer_settings.hpp>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#include "monitor_overlay.h"
#include "monitor_present.h"

#ifdef __ANDROID__
#include <android/log.h>
#endif

#if defined(_WIN32) && !defined(NDEBUG)
#include <crtdbg.h>
#endif

struct monitor_layer_data {
    VkuDeviceDispatchTable *device_dispatch_table{};
    VkuInstanceDispatchTable *instance_dispatch_table{};

    VkInstance instance{};
    VkPhysicalDevice gpu{};
    VkDevice device{};

    PFN_vkSetDeviceLoaderData pfn_dev_init{};

    // Instance state for the overlay
    std::atomic<uint64_t> frame_counter{0};
    monitor::MonitorSettings settings;

    // Device state for the overlay, guarded by `lock`
    std::mutex lock;
    struct monitor_layer_data *instance_data{};
    std::vector<VkQueueFamilyProperties> queue_families;
    std::unordered_map<VkQueue, uint32_t> queue_family_index;
    std::unordered_map<VkSwapchainKHR, std::unique_ptr<monitor::SwapchainOverlay>> swapchains;
    std::unordered_map<uint32_t, monitor::SlotRing> slot_rings;  // By queue family
    monitor::PresentRewrite present_rewrite;
};

// Instances and devices are created and destroyed from any thread, so the map is always locked.
static std::mutex layer_data_lock;
static std::unordered_map<void *, monitor_layer_data *> layer_data_map;

template monitor_layer_data *GetLayerDataPtr<monitor_layer_data>(void *data_key,
                                                                 std::unordered_map<void *, monitor_layer_data *> &data_map);

static monitor_layer_data *GetLayerData(void *key) {
    const std::scoped_lock lock(layer_data_lock);
    return GetLayerDataPtr(key, layer_data_map);
}

// Each distinct message is only shown once
static void WarnOnce(const std::string &message) {
    static std::mutex lock;
    static std::set<std::string> shown;
    const std::scoped_lock guard(lock);
    if (!shown.insert(message).second) {
        return;
    }
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_WARN, "VK_LAYER_LUNARG_monitor", "%s", message.c_str());
#else
    (void)fprintf(stderr, "VK_LAYER_LUNARG_monitor: %s\n", message.c_str());
#endif
}

static void FreeLayerData(void *key) {
    const std::scoped_lock lock(layer_data_lock);
    auto it = layer_data_map.find(key);
    if (it != layer_data_map.end()) {
        delete it->second;
        layer_data_map.erase(it);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice gpu, const VkDeviceCreateInfo *pCreateInfo,
                                              const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
    VkLayerDeviceCreateInfo *chain_info = get_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);

    assert(chain_info->u.pLayerInfo);
    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr = chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    // Physical devices share their instance's dispatch key
    monitor_layer_data *instance_data = GetLayerData(get_dispatch_key(gpu));
    auto fpCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(fpGetInstanceProcAddr(instance_data->instance, "vkCreateDevice"));
    if (fpCreateDevice == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Advance the link info for the next element on the chain
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    VkResult result = fpCreateDevice(gpu, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) {
        return result;
    }

    monitor_layer_data *my_device_data = GetLayerData(get_dispatch_key(*pDevice));

    // Setup device dispatch table
    my_device_data->device_dispatch_table = new VkuDeviceDispatchTable;
    vkuInitDeviceDispatchTable(*pDevice, my_device_data->device_dispatch_table, fpGetDeviceProcAddr);

    // store the loader callback for initializing created dispatchable objects
    chain_info = get_chain_info(pCreateInfo, VK_LOADER_DATA_CALLBACK);
    if (chain_info) {
        my_device_data->pfn_dev_init = chain_info->u.pfnSetDeviceLoaderData;
    } else {
        my_device_data->pfn_dev_init = NULL;
    }

    my_device_data->gpu = gpu;
    my_device_data->device = *pDevice;
    my_device_data->instance_data = instance_data;

    uint32_t family_count = 0;
    instance_data->instance_dispatch_table->GetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, nullptr);
    my_device_data->queue_families.resize(family_count);
    instance_data->instance_dispatch_table->GetPhysicalDeviceQueueFamilyProperties(gpu, &family_count,
                                                                                   my_device_data->queue_families.data());
    return result;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
    dispatch_key key = get_dispatch_key(device);
    monitor_layer_data *my_data = GetLayerData(key);
    VkuDeviceDispatchTable *pTable = my_data->device_dispatch_table;
    pTable->DeviceWaitIdle(device);
    {
        const std::scoped_lock lock(my_data->lock);
        for (auto &swapchain : my_data->swapchains) {
            monitor::DestroySwapchainOverlay(device, *pTable, *swapchain.second);
        }
        my_data->swapchains.clear();
        for (auto &ring : my_data->slot_rings) {
            monitor::DestroySlotRing(device, *pTable, ring.second);
        }
        my_data->slot_rings.clear();
    }
    pTable->DestroyDevice(device, pAllocator);
    delete pTable;
    FreeLayerData(key);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
                                                VkInstance *pInstance) {
#if defined(_WIN32) && defined(_CRTDBG_MODE_FILE)
#if !defined(NDEBUG)
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    // Avoid "Abort, Retry, Ignore" dialog boxes
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif

    VkLayerInstanceCreateInfo *chain_info = get_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);

    assert(chain_info->u.pLayerInfo);
    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkCreateInstance fpCreateInstance = (PFN_vkCreateInstance)fpGetInstanceProcAddr(NULL, "vkCreateInstance");
    if (fpCreateInstance == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Advance the link info for the next element on the chain
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    VkResult result = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) return result;

    monitor_layer_data *my_data = GetLayerData(get_dispatch_key(*pInstance));
    my_data->instance = *pInstance;
    my_data->instance_dispatch_table = new VkuInstanceDispatchTable;
    vkuInitInstanceDispatchTable(*pInstance, my_data->instance_dispatch_table, fpGetInstanceProcAddr);

    return result;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator) {
    dispatch_key key = get_dispatch_key(instance);
    const monitor_layer_data *my_data = GetLayerData(key);
    VkuInstanceDispatchTable *pTable = my_data->instance_dispatch_table;
    pTable->DestroyInstance(instance, pAllocator);
    delete pTable;
    FreeLayerData(key);
}

// Draws the overlay on every swapchain being presented, in one submit that waits for the app's work. The present then
// waits for that submit instead.
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo) {
    monitor_layer_data *my_data = GetLayerData(get_dispatch_key(queue));
    monitor_layer_data *instance_data = my_data->instance_data;
    const VkuDeviceDispatchTable &dispatch = *my_data->device_dispatch_table;
    const monitor::MonitorSettings &settings = instance_data->settings;
    const uint64_t frame_number = monitor::NextFrameNumber(instance_data->frame_counter);
    const auto now = std::chrono::steady_clock::now();

    const std::scoped_lock lock(my_data->lock);

    // Statistics are kept for every swapchain, even ones without an overlay
    std::vector<monitor::SwapchainOverlay *> overlays(pPresentInfo->swapchainCount, nullptr);
    std::vector<bool> drawn(pPresentInfo->swapchainCount, false);
    bool any_overlay = false;
    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
        auto it = my_data->swapchains.find(pPresentInfo->pSwapchains[i]);
        if (it == my_data->swapchains.end()) {
            continue;
        }
        monitor::RecordPresent(it->second->stats, now, settings.update_interval);
        if (it->second->ok && monitor::PrepareImage(my_data->device, dispatch, *it->second, pPresentInfo->pImageIndices[i])) {
            overlays[i] = it->second.get();
            drawn[i] = true;
            any_overlay = true;
        }
    }

    const auto family = my_data->queue_family_index.find(queue);
    const bool known_family = family != my_data->queue_family_index.end() && family->second < my_data->queue_families.size();
    const VkQueueFlags queue_flags = known_family ? my_data->queue_families[family->second].queueFlags : 0;
    const bool any_line = settings.lines.fps || settings.lines.frame_time || settings.lines.frame_number;
    if (!known_family || !monitor::ShouldDrawOverlay(queue_flags, any_overlay, any_line)) {
        if (any_overlay && any_line && ((queue_flags & VK_QUEUE_GRAPHICS_BIT) == 0u)) {
            WarnOnce("no overlay when presenting from a queue without graphics");
        }
        return dispatch.QueuePresentKHR(queue, pPresentInfo);
    }

    monitor::SlotRing &ring = my_data->slot_rings[family->second];
    if (!ring.ok && !ring.reason) {
        monitor::CreateSlotRing(my_data->device, dispatch, family->second, my_data->pfn_dev_init, 3, ring);
    }
    if (!ring.ok) {
        WarnOnce(std::string("no overlay, ") + (ring.reason ? ring.reason : "could not create command buffers"));
        return dispatch.QueuePresentKHR(queue, pPresentInfo);
    }

    monitor::SubmitSlot &slot = monitor::AcquireSlot(my_data->device, dispatch, ring);
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dispatch.BeginCommandBuffer(slot.command_buffer, &begin) != VK_SUCCESS) {
        WarnOnce("out of memory, some frames are presented without the overlay");
        return dispatch.QueuePresentKHR(queue, pPresentInfo);
    }

    std::vector<VkSemaphore> signals;
    std::vector<VkSwapchainKHR> swapchains;
    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
        monitor::SwapchainOverlay *overlay = overlays[i];
        if (!overlay) {
            continue;
        }
        const monitor::SwapchainImage &image = overlay->images[pPresentInfo->pImageIndices[i]];

        const std::vector<std::string> lines = monitor::FormatOverlayLines(overlay->stats.snapshot, frame_number, settings.lines);
        const monitor::OverlayLayout layout =
            monitor::ComputeOverlayLayout(overlay->extent, settings.position, settings.scale_percent, overlay->transform, lines);
        const monitor::WhiteLevel white = monitor::WhiteForColorSpace(overlay->color_space);
        if (layout.transform_warning) {
            WarnOnce("mirrored swapchain transforms are drawn without the mirror");
        }
        if (!white.known) {
            WarnOnce("unknown swapchain color space, the overlay may be too bright or too dark");
        }

        const monitor::OverlayPushConstants push_constants =
            monitor::PackOverlayPushConstants(layout, lines, white.value, !overlay->blend);
        monitor::RecordOverlay(dispatch, slot.command_buffer, overlay->pipeline, image.framebuffer, overlay->extent,
                               layout.image_rect, push_constants);
        signals.push_back(image.present_wait);
        swapchains.push_back(overlay->swapchain);
    }
    if (dispatch.EndCommandBuffer(slot.command_buffer) != VK_SUCCESS) {
        WarnOnce("out of memory, some frames are presented without the overlay");
        return dispatch.QueuePresentKHR(queue, pPresentInfo);
    }

    const std::vector<VkSemaphore> waits(pPresentInfo->pWaitSemaphores,
                                         pPresentInfo->pWaitSemaphores + pPresentInfo->waitSemaphoreCount);
    VkResult result = monitor::SubmitOverlay(my_data->device, dispatch, queue, slot, waits, signals, swapchains);
    if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        // The app's semaphores weren't touched, so the frame can still be presented without the overlay
        WarnOnce("out of memory, some frames are presented without the overlay");
        return dispatch.QueuePresentKHR(queue, pPresentInfo);
    }
    if (result != VK_SUCCESS) {
        return result;
    }

    const VkPresentInfoKHR &present_info = monitor::RewritePresentInfo(*pPresentInfo, signals, drawn, my_data->present_rewrite);
    result = dispatch.QueuePresentKHR(queue, &present_info);
    if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        // Nothing waited on the overlay semaphores, so unsignal them before they are used again
        monitor::DrainSemaphores(dispatch, queue, signals);
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceToolPropertiesEXT(VkPhysicalDevice physicalDevice, uint32_t *pToolCount,
                                                                    VkPhysicalDeviceToolPropertiesEXT *pToolProperties) {
    static const VkPhysicalDeviceToolPropertiesEXT monitor_layer_tool_props = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TOOL_PROPERTIES_EXT,
        nullptr,
        "Monitor Layer",
        "2",
        VK_TOOL_PURPOSE_PROFILING_BIT_EXT | VK_TOOL_PURPOSE_ADDITIONAL_FEATURES_BIT_EXT,
        "The VK_LAYER_LUNARG_monitor utility layer draws the frame rate, frame time and frame number on top of the presented "
        "images.",
        "VK_LAYER_LUNARG_monitor"};

    auto original_pToolProperties = pToolProperties;
    if (pToolProperties != nullptr) {
        *pToolProperties = monitor_layer_tool_props;
        pToolProperties = ((*pToolCount > 1) ? &pToolProperties[1] : nullptr);
        (*pToolCount)--;
    }

    const monitor_layer_data *my_data = GetLayerData(get_dispatch_key(physicalDevice));
    VkResult result =
        my_data->instance_dispatch_table->GetPhysicalDeviceToolPropertiesEXT(physicalDevice, pToolCount, pToolProperties);

    if (original_pToolProperties != nullptr) {
        pToolProperties = original_pToolProperties;
    }

    (*pToolCount)++;

    return result;
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue) {
    monitor_layer_data *my_data = GetLayerData(get_dispatch_key(device));
    my_data->device_dispatch_table->GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);

    const std::scoped_lock lock(my_data->lock);
    my_data->queue_family_index[*pQueue] = queueFamilyIndex;
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *pQueueInfo, VkQueue *pQueue) {
    monitor_layer_data *my_data = GetLayerData(get_dispatch_key(device));
    my_data->device_dispatch_table->GetDeviceQueue2(device, pQueueInfo, pQueue);

    const std::scoped_lock lock(my_data->lock);
    my_data->queue_family_index[*pQueue] = pQueueInfo->queueFamilyIndex;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
                                                    const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain) {
    monitor_layer_data *my_data = GetLayerData(get_dispatch_key(device));
    const VkuDeviceDispatchTable &dispatch = *my_data->device_dispatch_table;

    // The overlay renders into the swapchain images, so they need to be color attachments
    const monitor::Eligibility eligibility = monitor::SwapchainOverlayEligibility(*pCreateInfo);
    const VkSwapchainCreateInfoKHR create_info = eligibility.ok ? monitor::PatchSwapchainCreateInfo(*pCreateInfo) : *pCreateInfo;
    const VkResult result = dispatch.CreateSwapchainKHR(device, &create_info, pAllocator, pSwapchain);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkFormatProperties format_properties = {};
    my_data->instance_data->instance_dispatch_table->GetPhysicalDeviceFormatProperties(my_data->gpu, create_info.imageFormat,
                                                                                       &format_properties);
    const bool blend = (format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0;

    auto overlay = std::make_unique<monitor::SwapchainOverlay>();
    monitor::SetupSwapchainOverlay(device, dispatch, *pSwapchain, create_info, blend, *overlay);
    if (!overlay->ok) {
        WarnOnce(std::string("no overlay on some swapchains, ") + overlay->reason);
    }

    const std::scoped_lock lock(my_data->lock);
    my_data->swapchains[*pSwapchain] = std::move(overlay);
    return result;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                 const VkAllocationCallbacks *pAllocator) {
    monitor_layer_data *my_data = GetLayerData(get_dispatch_key(device));
    const VkuDeviceDispatchTable &dispatch = *my_data->device_dispatch_table;
    {
        const std::scoped_lock lock(my_data->lock);
        auto it = my_data->swapchains.find(swapchain);
        if (it != my_data->swapchains.end()) {
            for (auto &ring : my_data->slot_rings) {
                monitor::WaitForSwapchain(device, dispatch, ring.second, swapchain);
            }
            monitor::DestroySwapchainOverlay(device, dispatch, *it->second);
            my_data->swapchains.erase(it);
        }
    }
    dispatch.DestroySwapchainKHR(device, swapchain, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSharedSwapchainsKHR(VkDevice device, uint32_t swapchainCount,
                                                           const VkSwapchainCreateInfoKHR *pCreateInfos,
                                                           const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchains) {
    WarnOnce("no overlay on swapchains from vkCreateSharedSwapchainsKHR");
    const monitor_layer_data *my_data = GetLayerData(get_dispatch_key(device));
    return my_data->device_dispatch_table->CreateSharedSwapchainsKHR(device, swapchainCount, pCreateInfos, pAllocator, pSwapchains);
}

#if defined(__GNUC__) && __GNUC__ >= 4
#define EXPORT_FUNCTION __attribute__((visibility("default")))
#elif defined(__SUNPRO_C) && (__SUNPRO_C >= 0x590)
#define EXPORT_FUNCTION __attribute__((visibility("default")))
#else
#define EXPORT_FUNCTION
#endif

EXPORT_FUNCTION VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice dev, const char *funcName) {
#define ADD_HOOK(fn) \
    if (!strncmp(#fn, funcName, sizeof(#fn))) return reinterpret_cast<PFN_vkVoidFunction>(fn)

    ADD_HOOK(vkGetDeviceProcAddr);
    ADD_HOOK(vkDestroyDevice);
    ADD_HOOK(vkQueuePresentKHR);
    ADD_HOOK(vkGetDeviceQueue);
#undef ADD_HOOK

    if (dev == NULL) return NULL;

    monitor_layer_data *dev_data;
    dev_data = GetLayerData(get_dispatch_key(dev));
    VkuDeviceDispatchTable *pTable = dev_data->device_dispatch_table;

    if (pTable->GetDeviceProcAddr == NULL) return NULL;
    PFN_vkVoidFunction next = pTable->GetDeviceProcAddr(dev, funcName);

    // Functions from extensions or newer versions are only hooked when the device has them
#define ADD_HOOK_IF_SUPPORTED(fn) \
    if (next && !strncmp(#fn, funcName, sizeof(#fn))) return reinterpret_cast<PFN_vkVoidFunction>(fn)

    ADD_HOOK_IF_SUPPORTED(vkGetDeviceQueue2);
    ADD_HOOK_IF_SUPPORTED(vkCreateSwapchainKHR);
    ADD_HOOK_IF_SUPPORTED(vkDestroySwapchainKHR);
    ADD_HOOK_IF_SUPPORTED(vkCreateSharedSwapchainsKHR);
#undef ADD_HOOK_IF_SUPPORTED

    return next;
}

EXPORT_FUNCTION VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *funcName) {
#define ADD_HOOK(fn) \
    if (!strncmp(#fn, funcName, sizeof(#fn))) return reinterpret_cast<PFN_vkVoidFunction>(fn)

    ADD_HOOK(vkCreateInstance);
    ADD_HOOK(vkCreateDevice);
    ADD_HOOK(vkDestroyInstance);
    ADD_HOOK(vkGetInstanceProcAddr);
    ADD_HOOK(vkGetPhysicalDeviceToolPropertiesEXT);
#undef ADD_HOOK

    if (instance == NULL) return NULL;

    monitor_layer_data *instance_data;
    instance_data = GetLayerData(get_dispatch_key(instance));
    VkuInstanceDispatchTable *pTable = instance_data->instance_dispatch_table;

    if (pTable->GetInstanceProcAddr == NULL) return NULL;
    return pTable->GetInstanceProcAddr(instance, funcName);
}
