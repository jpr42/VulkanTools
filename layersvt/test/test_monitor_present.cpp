/*
 * Copyright (c) 2026 LunarG, Inc.
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
 */

// Tests the swapchain and present plumbing against a fake device: a dispatch table of stubs that count every object
// created and destroyed, record submissions, and fail on request. No Vulkan driver is needed.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "monitor_present.h"

namespace {

struct FakeDevice;
FakeDevice* fake = nullptr;

template <typename T>
T MakeHandle(uint64_t value) {
    static_assert(sizeof(T) == sizeof(uint64_t), "non-dispatchable handles are 64 bits");
    T handle{};
    memcpy(static_cast<void*>(&handle), &value, sizeof(T));
    return handle;
}

template <typename T>
uint64_t HandleValue(T handle) {
    uint64_t value = 0;
    memcpy(&value, static_cast<const void*>(&handle), sizeof(value));
    return value;
}

struct Submission {
    std::vector<VkSemaphore> waits;
    std::vector<VkPipelineStageFlags> wait_stages;
    std::vector<VkSemaphore> signals;
    uint32_t command_buffer_count = 0;
    VkFence fence = VK_NULL_HANDLE;
};

struct FakeDevice {
    std::map<std::string, int> created;
    std::map<std::string, int> destroyed;
    std::map<std::string, int> attempts;
    std::map<std::string, int> fail_at;  // Fail the Nth create attempt (1-based) of a kind of object
    uint64_t next_handle = 0x1000;
    uint32_t image_count = 3;

    // Fences are signaled by a successful submit. Waiting on one that is unsignaled and was never submitted would hang.
    std::map<uint64_t, bool> fence_signaled;
    std::set<uint64_t> fences_waited;
    int hung_waits = 0;
    VkResult submit_result = VK_SUCCESS;
    std::vector<Submission> submissions;

    // Command buffers are dispatchable, so they get real memory for the loader's dispatch pointer.
    std::vector<std::unique_ptr<uint64_t>> command_buffers;
    int loader_data_calls = 0;

    FakeDevice() { fake = this; }
    ~FakeDevice() { fake = nullptr; }
    FakeDevice(const FakeDevice&) = delete;
    FakeDevice& operator=(const FakeDevice&) = delete;
    FakeDevice(FakeDevice&&) = delete;
    FakeDevice& operator=(FakeDevice&&) = delete;

    int Live(const std::string& kind) { return created[kind] - destroyed[kind]; }

    template <typename T>
    VkResult Create(const std::string& kind, T* out) {
        const int attempt = ++attempts[kind];
        if (fail_at.count(kind) && fail_at[kind] == attempt) {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        ++created[kind];
        *out = MakeHandle<T>(next_handle++);
        return VK_SUCCESS;
    }

    template <typename T>
    void Destroy(const std::string& kind, T handle) {
        if (HandleValue(handle) != 0) {
            ++destroyed[kind];
        }
    }

    static VkuDeviceDispatchTable Dispatch();
};

#define STUB_CREATE(Name, Info, Type, kind)                                                                     \
    VKAPI_ATTR VkResult VKAPI_CALL Stub##Name(VkDevice, const Info*, const VkAllocationCallbacks*, Type* out) { \
        return fake->Create(kind, out);                                                                         \
    }
#define STUB_DESTROY(Name, Type, kind) \
    VKAPI_ATTR void VKAPI_CALL Stub##Name(VkDevice, Type handle, const VkAllocationCallbacks*) { fake->Destroy(kind, handle); }

STUB_CREATE(CreateImageView, VkImageViewCreateInfo, VkImageView, "view")
STUB_DESTROY(DestroyImageView, VkImageView, "view")
STUB_CREATE(CreateFramebuffer, VkFramebufferCreateInfo, VkFramebuffer, "framebuffer")
STUB_DESTROY(DestroyFramebuffer, VkFramebuffer, "framebuffer")
STUB_CREATE(CreateSemaphore, VkSemaphoreCreateInfo, VkSemaphore, "semaphore")
STUB_DESTROY(DestroySemaphore, VkSemaphore, "semaphore")
STUB_CREATE(CreateRenderPass, VkRenderPassCreateInfo, VkRenderPass, "render pass")
STUB_DESTROY(DestroyRenderPass, VkRenderPass, "render pass")
STUB_CREATE(CreatePipelineLayout, VkPipelineLayoutCreateInfo, VkPipelineLayout, "pipeline layout")
STUB_DESTROY(DestroyPipelineLayout, VkPipelineLayout, "pipeline layout")
STUB_CREATE(CreateShaderModule, VkShaderModuleCreateInfo, VkShaderModule, "shader module")
STUB_DESTROY(DestroyShaderModule, VkShaderModule, "shader module")
STUB_DESTROY(DestroyPipeline, VkPipeline, "pipeline")
STUB_CREATE(CreateCommandPool, VkCommandPoolCreateInfo, VkCommandPool, "command pool")
STUB_DESTROY(DestroyCommandPool, VkCommandPool, "command pool")
STUB_DESTROY(DestroyFence, VkFence, "fence")

VKAPI_ATTR VkResult VKAPI_CALL StubCreateGraphicsPipelines(VkDevice /*device*/, VkPipelineCache /*pipelineCache*/, uint32_t count,
                                                           const VkGraphicsPipelineCreateInfo* /*pCreateInfos*/,
                                                           const VkAllocationCallbacks* /*pAllocator*/, VkPipeline* out) {
    for (uint32_t i = 0; i < count; ++i) {
        const VkResult result = fake->Create("pipeline", &out[i]);
        if (result != VK_SUCCESS) {
            return result;
        }
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL StubGetSwapchainImagesKHR(VkDevice /*device*/, VkSwapchainKHR /*swapchain*/, uint32_t* count,
                                                         VkImage* images) {
    if (!images) {
        *count = fake->image_count;
        return VK_SUCCESS;
    }
    for (uint32_t i = 0; i < *count && i < fake->image_count; ++i) {
        images[i] = MakeHandle<VkImage>(0x100 + i);
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL StubCreateFence(VkDevice /*device*/, const VkFenceCreateInfo* info,
                                               const VkAllocationCallbacks* /*pAllocator*/, VkFence* out) {
    const VkResult result = fake->Create("fence", out);
    if (result == VK_SUCCESS) {
        fake->fence_signaled[HandleValue(*out)] = (info->flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0;
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL StubWaitForFences(VkDevice /*device*/, uint32_t count, const VkFence* fences, VkBool32 /*waitAll*/,
                                                 uint64_t /*timeout*/) {
    for (uint32_t i = 0; i < count; ++i) {
        fake->fences_waited.insert(HandleValue(fences[i]));
        if (!fake->fence_signaled[HandleValue(fences[i])]) {
            ++fake->hung_waits;
        }
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL StubResetFences(VkDevice /*device*/, uint32_t count, const VkFence* fences) {
    for (uint32_t i = 0; i < count; ++i) {
        fake->fence_signaled[HandleValue(fences[i])] = false;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL StubAllocateCommandBuffers(VkDevice /*device*/, const VkCommandBufferAllocateInfo* info,
                                                          VkCommandBuffer* out) {
    for (uint32_t i = 0; i < info->commandBufferCount; ++i) {
        fake->command_buffers.push_back(std::make_unique<uint64_t>(0));
        out[i] = reinterpret_cast<VkCommandBuffer>(fake->command_buffers.back().get());
        ++fake->created["command buffer"];
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL StubFreeCommandBuffers(VkDevice /*device*/, VkCommandPool /*commandPool*/, uint32_t count,
                                                  const VkCommandBuffer* buffers) {
    for (uint32_t i = 0; i < count; ++i) {
        if (buffers[i]) {
            ++fake->destroyed["command buffer"];
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL StubQueueSubmit(VkQueue /*queue*/, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    if (fake->submit_result != VK_SUCCESS) {
        return fake->submit_result;
    }
    for (uint32_t i = 0; i < count; ++i) {
        Submission submission;
        submission.waits.assign(submits[i].pWaitSemaphores, submits[i].pWaitSemaphores + submits[i].waitSemaphoreCount);
        submission.wait_stages.assign(submits[i].pWaitDstStageMask, submits[i].pWaitDstStageMask + submits[i].waitSemaphoreCount);
        submission.signals.assign(submits[i].pSignalSemaphores, submits[i].pSignalSemaphores + submits[i].signalSemaphoreCount);
        submission.command_buffer_count = submits[i].commandBufferCount;
        submission.fence = fence;
        fake->submissions.push_back(submission);
    }
    if (fence) {
        fake->fence_signaled[HandleValue(fence)] = true;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL StubBeginCommandBuffer(VkCommandBuffer /*commandBuffer*/,
                                                      const VkCommandBufferBeginInfo* /*pBeginInfo*/) {
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL StubEndCommandBuffer(VkCommandBuffer /*commandBuffer*/) { return VK_SUCCESS; }

VkuDeviceDispatchTable FakeDevice::Dispatch() {
    VkuDeviceDispatchTable dispatch = {};
    dispatch.CreateImageView = StubCreateImageView;
    dispatch.DestroyImageView = StubDestroyImageView;
    dispatch.CreateFramebuffer = StubCreateFramebuffer;
    dispatch.DestroyFramebuffer = StubDestroyFramebuffer;
    dispatch.CreateSemaphore = StubCreateSemaphore;
    dispatch.DestroySemaphore = StubDestroySemaphore;
    dispatch.CreateRenderPass = StubCreateRenderPass;
    dispatch.DestroyRenderPass = StubDestroyRenderPass;
    dispatch.CreatePipelineLayout = StubCreatePipelineLayout;
    dispatch.DestroyPipelineLayout = StubDestroyPipelineLayout;
    dispatch.CreateShaderModule = StubCreateShaderModule;
    dispatch.DestroyShaderModule = StubDestroyShaderModule;
    dispatch.CreateGraphicsPipelines = StubCreateGraphicsPipelines;
    dispatch.DestroyPipeline = StubDestroyPipeline;
    dispatch.GetSwapchainImagesKHR = StubGetSwapchainImagesKHR;
    dispatch.CreateCommandPool = StubCreateCommandPool;
    dispatch.DestroyCommandPool = StubDestroyCommandPool;
    dispatch.AllocateCommandBuffers = StubAllocateCommandBuffers;
    dispatch.FreeCommandBuffers = StubFreeCommandBuffers;
    dispatch.CreateFence = StubCreateFence;
    dispatch.DestroyFence = StubDestroyFence;
    dispatch.WaitForFences = StubWaitForFences;
    dispatch.ResetFences = StubResetFences;
    dispatch.QueueSubmit = StubQueueSubmit;
    dispatch.BeginCommandBuffer = StubBeginCommandBuffer;
    dispatch.EndCommandBuffer = StubEndCommandBuffer;
    return dispatch;
}

uint64_t device_object = 0;
uint64_t queue_object = 0;
const VkDevice kDevice = reinterpret_cast<VkDevice>(&device_object);
const VkQueue kQueue = reinterpret_cast<VkQueue>(&queue_object);

VkSwapchainCreateInfoKHR SwapchainInfo() {
    VkSwapchainCreateInfoKHR info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.imageFormat = VK_FORMAT_B8G8R8A8_SRGB;
    info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    info.imageExtent = {1920, 1080};
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    return info;
}

const char* kKinds[] = {"view",     "framebuffer",   "semaphore",    "render pass",    "pipeline layout",
                        "pipeline", "shader module", "command pool", "command buffer", "fence"};

void ExpectNothingLive(FakeDevice& device) {
    for (const char* kind : kKinds) {
        EXPECT_EQ(device.Live(kind), 0) << kind;
    }
}

TEST(MonitorPresent, SwapchainSetupCreatesObjectsPerImageAndTeardownFreesThem) {
    FakeDevice device;
    const VkuDeviceDispatchTable dispatch = FakeDevice::Dispatch();
    monitor::SwapchainOverlay overlay;
    monitor::SetupSwapchainOverlay(kDevice, dispatch, MakeHandle<VkSwapchainKHR>(0x50), SwapchainInfo(), true, overlay);

    EXPECT_TRUE(overlay.ok);
    EXPECT_EQ(overlay.images.size(), 3u);
    EXPECT_EQ(device.Live("view"), 3);
    EXPECT_EQ(device.Live("framebuffer"), 3);
    EXPECT_EQ(device.Live("semaphore"), 3);
    EXPECT_EQ(device.Live("pipeline"), 1);
    EXPECT_EQ(device.Live("shader module"), 0);

    monitor::DestroySwapchainOverlay(kDevice, dispatch, overlay);
    ExpectNothingLive(device);
}

TEST(MonitorPresent, DeferredSwapchainCreatesViewsOnFirstPresentOfEachImage) {
    FakeDevice device;
    const VkuDeviceDispatchTable dispatch = FakeDevice::Dispatch();
    VkSwapchainCreateInfoKHR info = SwapchainInfo();
    info.flags = VK_SWAPCHAIN_CREATE_DEFERRED_MEMORY_ALLOCATION_BIT_KHR;
    monitor::SwapchainOverlay overlay;
    monitor::SetupSwapchainOverlay(kDevice, dispatch, MakeHandle<VkSwapchainKHR>(0x50), info, true, overlay);

    EXPECT_TRUE(overlay.ok);
    EXPECT_EQ(device.Live("semaphore"), 3);
    EXPECT_EQ(device.Live("view"), 0);  // The images may have no memory yet

    EXPECT_TRUE(monitor::PrepareImage(kDevice, dispatch, overlay, 2));
    EXPECT_EQ(device.Live("view"), 1);
    EXPECT_TRUE(monitor::PrepareImage(kDevice, dispatch, overlay, 2));
    EXPECT_EQ(device.Live("view"), 1);
    EXPECT_TRUE(monitor::PrepareImage(kDevice, dispatch, overlay, 0));
    EXPECT_TRUE(monitor::PrepareImage(kDevice, dispatch, overlay, 1));
    EXPECT_EQ(device.Live("view"), 3);
    EXPECT_EQ(device.Live("framebuffer"), 3);
    EXPECT_FALSE(monitor::PrepareImage(kDevice, dispatch, overlay, 3));

    monitor::DestroySwapchainOverlay(kDevice, dispatch, overlay);
    ExpectNothingLive(device);
}

TEST(MonitorPresent, FailedSwapchainSetupDisablesOverlayWithoutLeaking) {
    // Fail part way through the per image objects, or the one-off pipeline objects
    const std::pair<const char*, int> failures[] = {
        {"view", 2}, {"framebuffer", 2}, {"semaphore", 2}, {"render pass", 1}, {"pipeline", 1}};
    for (const auto& failure : failures) {
        const char* kind = failure.first;
        FakeDevice device;
        device.fail_at[kind] = failure.second;
        const VkuDeviceDispatchTable dispatch = FakeDevice::Dispatch();
        monitor::SwapchainOverlay overlay;
        monitor::SetupSwapchainOverlay(kDevice, dispatch, MakeHandle<VkSwapchainKHR>(0x50), SwapchainInfo(), true, overlay);

        EXPECT_FALSE(overlay.ok) << kind;
        EXPECT_NE(overlay.reason, nullptr) << kind;
        monitor::DestroySwapchainOverlay(kDevice, dispatch, overlay);
        ExpectNothingLive(device);
    }
}

TEST(MonitorPresent, IneligibleSwapchainCreatesNothing) {
    FakeDevice device;
    const VkuDeviceDispatchTable dispatch = FakeDevice::Dispatch();
    VkSwapchainCreateInfoKHR info = SwapchainInfo();
    info.presentMode = VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR;
    monitor::SwapchainOverlay overlay;
    monitor::SetupSwapchainOverlay(kDevice, dispatch, MakeHandle<VkSwapchainKHR>(0x50), info, true, overlay);

    EXPECT_FALSE(overlay.ok);
    EXPECT_NE(overlay.reason, nullptr);
    for (const char* kind : kKinds) {
        EXPECT_EQ(device.created[kind], 0) << kind;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL SetLoaderData(VkDevice /*device*/, void* object) {
    ++fake->loader_data_calls;
    *static_cast<uint64_t*>(object) = 0x10ADE7;  // Where the loader keeps its dispatch pointer
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL FailLoaderData(VkDevice /*device*/, void* /*object*/) {
    ++fake->loader_data_calls;
    return VK_ERROR_INITIALIZATION_FAILED;
}

const uint32_t kSlots = 3;

TEST(MonitorPresent, SlotRingGivesEveryCommandBufferLoaderDispatch) {
    FakeDevice device;
    const VkuDeviceDispatchTable dispatch = FakeDevice::Dispatch();
    monitor::SlotRing ring;
    monitor::CreateSlotRing(kDevice, dispatch, 0, SetLoaderData, kSlots, ring);

    EXPECT_TRUE(ring.ok);
    EXPECT_EQ(device.Live("command buffer"), static_cast<int>(kSlots));
    EXPECT_EQ(device.loader_data_calls, static_cast<int>(kSlots));
    for (const monitor::SubmitSlot& slot : ring.slots) {
        EXPECT_EQ(*reinterpret_cast<uint64_t*>(slot.command_buffer), 0x10ADE7u);
    }

    monitor::DestroySlotRing(kDevice, dispatch, ring);
    ExpectNothingLive(device);
}

TEST(MonitorPresent, SlotRingIsDisabledWithoutLoaderDispatch) {
    for (PFN_vkSetDeviceLoaderData set_loader_data : {static_cast<PFN_vkSetDeviceLoaderData>(nullptr), FailLoaderData}) {
        FakeDevice device;
        const VkuDeviceDispatchTable dispatch = FakeDevice::Dispatch();
        monitor::SlotRing ring;
        monitor::CreateSlotRing(kDevice, dispatch, 0, set_loader_data, kSlots, ring);

        EXPECT_FALSE(ring.ok);
        EXPECT_NE(ring.reason, nullptr);
        monitor::DestroySlotRing(kDevice, dispatch, ring);
        ExpectNothingLive(device);
    }
}

// Takes the next slot and submits it with no semaphores, as one present would.
VkResult Present(monitor::SlotRing& ring, const VkuDeviceDispatchTable& dispatch, VkSwapchainKHR swapchain) {
    monitor::SubmitSlot& slot = monitor::AcquireSlot(kDevice, dispatch, ring);
    return monitor::SubmitOverlay(kDevice, dispatch, kQueue, slot, {}, {}, {swapchain});
}

TEST(MonitorPresent, SlotRingOnlyWaitsForSubmittedWork) {
    FakeDevice device;
    const VkuDeviceDispatchTable dispatch = FakeDevice::Dispatch();
    monitor::SlotRing ring;
    monitor::CreateSlotRing(kDevice, dispatch, 0, SetLoaderData, kSlots, ring);
    auto* const swapchain = MakeHandle<VkSwapchainKHR>(0x50);

    // The first trip around the ring has nothing to wait for
    for (uint32_t i = 0; i < kSlots; ++i) {
        ASSERT_EQ(Present(ring, dispatch, swapchain), VK_SUCCESS);
    }
    EXPECT_TRUE(device.fences_waited.empty());

    // Then each slot waits for its previous submission
    ASSERT_EQ(Present(ring, dispatch, swapchain), VK_SUCCESS);
    EXPECT_EQ(device.fences_waited.size(), 1u);
    EXPECT_EQ(device.hung_waits, 0);

    monitor::DestroySlotRing(kDevice, dispatch, ring);
    ExpectNothingLive(device);
}

TEST(MonitorPresent, FailedSubmitDoesNotLeaveAFenceToWaitOnForever) {
    FakeDevice device;
    const VkuDeviceDispatchTable dispatch = FakeDevice::Dispatch();
    monitor::SlotRing ring;
    monitor::CreateSlotRing(kDevice, dispatch, 0, SetLoaderData, kSlots, ring);
    auto* const swapchain = MakeHandle<VkSwapchainKHR>(0x50);

    for (uint32_t i = 0; i < kSlots; ++i) {
        ASSERT_EQ(Present(ring, dispatch, swapchain), VK_SUCCESS);
    }
    device.submit_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    EXPECT_EQ(Present(ring, dispatch, swapchain), VK_ERROR_OUT_OF_DEVICE_MEMORY);
    device.submit_result = VK_SUCCESS;

    for (uint32_t i = 0; i < kSlots + 1; ++i) {
        ASSERT_EQ(Present(ring, dispatch, swapchain), VK_SUCCESS);
    }
    EXPECT_EQ(device.hung_waits, 0);

    monitor::DestroySlotRing(kDevice, dispatch, ring);
    EXPECT_EQ(device.hung_waits, 0);
    ExpectNothingLive(device);
}

TEST(MonitorPresent, WaitForSwapchainOnlyWaitsForItsOwnSubmissions) {
    FakeDevice device;
    const VkuDeviceDispatchTable dispatch = FakeDevice::Dispatch();
    monitor::SlotRing ring;
    monitor::CreateSlotRing(kDevice, dispatch, 0, SetLoaderData, kSlots, ring);
    auto* const first = MakeHandle<VkSwapchainKHR>(0x50);
    auto* const second = MakeHandle<VkSwapchainKHR>(0x60);

    ASSERT_EQ(Present(ring, dispatch, first), VK_SUCCESS);
    ASSERT_EQ(Present(ring, dispatch, second), VK_SUCCESS);
    monitor::WaitForSwapchain(kDevice, dispatch, ring, first);

    EXPECT_EQ(device.fences_waited.size(), 1u);
    EXPECT_EQ(device.fences_waited.count(HandleValue(ring.slots[0].fence)), 1u);
    EXPECT_EQ(device.hung_waits, 0);

    monitor::DestroySlotRing(kDevice, dispatch, ring);
    ExpectNothingLive(device);
}

TEST(MonitorPresent, SubmitWaitsForTheAppAndSignalsThePresent) {
    FakeDevice device;
    const VkuDeviceDispatchTable dispatch = FakeDevice::Dispatch();
    monitor::SlotRing ring;
    monitor::CreateSlotRing(kDevice, dispatch, 0, SetLoaderData, kSlots, ring);
    const std::vector<VkSemaphore> app = {MakeHandle<VkSemaphore>(0xA1), MakeHandle<VkSemaphore>(0xA2)};
    const std::vector<VkSemaphore> ours = {MakeHandle<VkSemaphore>(0xB1)};

    monitor::SubmitSlot& slot = monitor::AcquireSlot(kDevice, dispatch, ring);
    ASSERT_EQ(monitor::SubmitOverlay(kDevice, dispatch, kQueue, slot, app, ours, {MakeHandle<VkSwapchainKHR>(0x50)}), VK_SUCCESS);

    ASSERT_EQ(device.submissions.size(), 1u);
    const Submission& submission = device.submissions[0];
    EXPECT_EQ(submission.waits, app);
    EXPECT_EQ(submission.wait_stages, std::vector<VkPipelineStageFlags>(2, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT));
    EXPECT_EQ(submission.signals, ours);
    EXPECT_EQ(submission.command_buffer_count, 1u);
    EXPECT_EQ(HandleValue(submission.fence), HandleValue(slot.fence));

    monitor::DestroySlotRing(kDevice, dispatch, ring);
    ExpectNothingLive(device);
}

struct AppPresent {
    VkSwapchainKHR swapchains[2] = {MakeHandle<VkSwapchainKHR>(0x50), MakeHandle<VkSwapchainKHR>(0x60)};
    uint32_t indices[2] = {1, 2};
    VkResult results[2] = {};
    VkSemaphore waits[1] = {MakeHandle<VkSemaphore>(0xA1)};
    VkRectLayerKHR rects[2] = {{{0, 0}, {16, 16}, 0}, {{32, 32}, {8, 8}, 0}};
    VkPresentRegionKHR regions[2] = {{1, &rects[0]}, {1, &rects[1]}};
    VkPresentRegionsKHR present_regions = {VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR, nullptr, 2, regions};
    VkPresentInfoKHR info = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};

    AppPresent() {
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = waits;
        info.swapchainCount = 2;
        info.pSwapchains = swapchains;
        info.pImageIndices = indices;
        info.pResults = results;
    }
};

const VkPresentRegionsKHR* FindRegions(const void* chain) {
    for (const auto* node = static_cast<const VkBaseInStructure*>(chain); node; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR) {
            return reinterpret_cast<const VkPresentRegionsKHR*>(node);
        }
    }
    return nullptr;
}

TEST(MonitorPresent, PresentWaitsOnTheOverlayInsteadOfTheApp) {
    const AppPresent app;
    const std::vector<VkSemaphore> ours = {MakeHandle<VkSemaphore>(0xB1), MakeHandle<VkSemaphore>(0xB2)};
    monitor::PresentRewrite rewrite;
    const VkPresentInfoKHR& info = monitor::RewritePresentInfo(app.info, ours, {true, true}, rewrite);

    ASSERT_EQ(info.waitSemaphoreCount, 2u);
    EXPECT_EQ(HandleValue(info.pWaitSemaphores[0]), 0xB1u);
    EXPECT_EQ(HandleValue(info.pWaitSemaphores[1]), 0xB2u);
    EXPECT_EQ(info.swapchainCount, 2u);
    EXPECT_EQ(info.pSwapchains, app.swapchains);
    EXPECT_EQ(info.pImageIndices, app.indices);
    EXPECT_EQ(info.pResults, app.results);
    EXPECT_EQ(info.pNext, nullptr);
    EXPECT_EQ(app.info.pWaitSemaphores, app.waits);  // The app's struct is untouched
}

TEST(MonitorPresent, PresentRegionsCoverTheWholeImageWhenTheOverlayIsDrawn) {
    AppPresent app;
    VkPresentIdKHR present_id = {VK_STRUCTURE_TYPE_PRESENT_ID_KHR, nullptr, 2, nullptr};
    VkDisplayPresentInfoKHR display = {VK_STRUCTURE_TYPE_DISPLAY_PRESENT_INFO_KHR, &app.present_regions};
    app.present_regions.pNext = &present_id;
    app.info.pNext = &display;

    monitor::PresentRewrite rewrite;
    const VkPresentInfoKHR& info = monitor::RewritePresentInfo(app.info, {MakeHandle<VkSemaphore>(0xB1)}, {false, true}, rewrite);

    // The display struct before the regions is still there, and so is the present ID after them
    const auto* first = static_cast<const VkBaseInStructure*>(info.pNext);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->sType, VK_STRUCTURE_TYPE_DISPLAY_PRESENT_INFO_KHR);
    const VkPresentRegionsKHR* regions = FindRegions(info.pNext);
    ASSERT_NE(regions, nullptr);
    EXPECT_NE(regions, &app.present_regions);
    EXPECT_EQ(regions->pNext, &present_id);

    // Only the swapchain with the overlay reports the whole image as changed
    ASSERT_EQ(regions->swapchainCount, 2u);
    EXPECT_EQ(regions->pRegions[0].rectangleCount, 1u);
    EXPECT_EQ(regions->pRegions[0].pRectangles, &app.rects[0]);
    EXPECT_EQ(regions->pRegions[1].rectangleCount, 0u);
    EXPECT_FALSE(rewrite.regions_unchanged);

    // The app's structs are untouched
    EXPECT_EQ(app.regions[1].rectangleCount, 1u);
    EXPECT_EQ(display.pNext, &app.present_regions);
}

struct UnknownStruct {
    VkStructureType sType = VK_STRUCTURE_TYPE_MAX_ENUM;
    const void* pNext = nullptr;
};

TEST(MonitorPresent, PresentRegionsAreLeftAloneBehindAnUnknownStruct) {
    AppPresent app;
    UnknownStruct unknown = {static_cast<VkStructureType>(0x7FFF0001), &app.present_regions};
    app.info.pNext = &unknown;

    monitor::PresentRewrite rewrite;
    const VkPresentInfoKHR& info = monitor::RewritePresentInfo(app.info, {MakeHandle<VkSemaphore>(0xB1)}, {true, true}, rewrite);

    EXPECT_EQ(info.pNext, &unknown);
    EXPECT_TRUE(rewrite.regions_unchanged);
}

TEST(MonitorPresent, PresentRewriteCanBeReused) {
    AppPresent with_regions;
    with_regions.info.pNext = &with_regions.present_regions;
    AppPresent without_regions;
    UnknownStruct unknown = {static_cast<VkStructureType>(0x7FFF0001), &without_regions.present_regions};
    without_regions.info.pNext = &unknown;

    monitor::PresentRewrite rewrite;
    monitor::RewritePresentInfo(without_regions.info, {MakeHandle<VkSemaphore>(0xB1)}, {true, true}, rewrite);
    EXPECT_TRUE(rewrite.regions_unchanged);

    const VkPresentInfoKHR& info =
        monitor::RewritePresentInfo(with_regions.info, {MakeHandle<VkSemaphore>(0xC1)}, {true, false}, rewrite);
    EXPECT_FALSE(rewrite.regions_unchanged);
    ASSERT_EQ(info.waitSemaphoreCount, 1u);
    EXPECT_EQ(HandleValue(info.pWaitSemaphores[0]), 0xC1u);
    const VkPresentRegionsKHR* regions = FindRegions(info.pNext);
    ASSERT_NE(regions, nullptr);
    EXPECT_EQ(regions->pRegions[0].rectangleCount, 0u);
    EXPECT_EQ(regions->pRegions[1].rectangleCount, 1u);

    const AppPresent no_chain;
    const VkPresentInfoKHR& plain = monitor::RewritePresentInfo(no_chain.info, {}, {true, true}, rewrite);
    EXPECT_EQ(plain.pNext, nullptr);
    EXPECT_EQ(plain.waitSemaphoreCount, 0u);
}

TEST(MonitorPresent, RetryingAFailedImageDoesNotLeak) {
    FakeDevice device;
    const VkuDeviceDispatchTable dispatch = FakeDevice::Dispatch();
    VkSwapchainCreateInfoKHR info = SwapchainInfo();
    info.flags = VK_SWAPCHAIN_CREATE_DEFERRED_MEMORY_ALLOCATION_BIT_KHR;
    monitor::SwapchainOverlay overlay;
    monitor::SetupSwapchainOverlay(kDevice, dispatch, MakeHandle<VkSwapchainKHR>(0x50), info, true, overlay);

    device.fail_at["framebuffer"] = 1;
    EXPECT_FALSE(monitor::PrepareImage(kDevice, dispatch, overlay, 0));
    EXPECT_TRUE(monitor::PrepareImage(kDevice, dispatch, overlay, 0));
    EXPECT_EQ(device.Live("view"), 1);

    monitor::DestroySwapchainOverlay(kDevice, dispatch, overlay);
    ExpectNothingLive(device);
}

TEST(MonitorPresent, SlotRingNeedsAtLeastOneSlot) {
    FakeDevice device;
    const VkuDeviceDispatchTable dispatch = FakeDevice::Dispatch();
    monitor::SlotRing ring;
    monitor::CreateSlotRing(kDevice, dispatch, 0, SetLoaderData, 0, ring);
    EXPECT_FALSE(ring.ok);
    monitor::DestroySlotRing(kDevice, dispatch, ring);
    ExpectNothingLive(device);
}

VKAPI_ATTR VkResult VKAPI_CALL FailResetFences(VkDevice /*device*/, uint32_t /*fenceCount*/, const VkFence* /*pFences*/) {
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

TEST(MonitorPresent, SubmitIsSkippedIfTheFenceCantBeReset) {
    FakeDevice device;
    VkuDeviceDispatchTable dispatch = FakeDevice::Dispatch();
    monitor::SlotRing ring;
    monitor::CreateSlotRing(kDevice, dispatch, 0, SetLoaderData, kSlots, ring);

    dispatch.ResetFences = FailResetFences;
    EXPECT_EQ(Present(ring, dispatch, MakeHandle<VkSwapchainKHR>(0x50)), VK_ERROR_OUT_OF_DEVICE_MEMORY);
    EXPECT_TRUE(device.submissions.empty());
    EXPECT_FALSE(ring.slots[0].in_flight);

    monitor::DestroySlotRing(kDevice, dispatch, ring);
    ExpectNothingLive(device);
}

TEST(MonitorPresent, DrainWithNoSemaphoresSubmitsNothing) {
    const FakeDevice device;
    const VkuDeviceDispatchTable dispatch = FakeDevice::Dispatch();
    EXPECT_EQ(monitor::DrainSemaphores(dispatch, kQueue, {}), VK_SUCCESS);
    EXPECT_TRUE(device.submissions.empty());
}

TEST(MonitorPresent, DrainUnsignalsTheOverlaySemaphores) {
    FakeDevice device;
    const VkuDeviceDispatchTable dispatch = FakeDevice::Dispatch();
    const std::vector<VkSemaphore> ours = {MakeHandle<VkSemaphore>(0xB1), MakeHandle<VkSemaphore>(0xB2)};
    EXPECT_EQ(monitor::DrainSemaphores(dispatch, kQueue, ours), VK_SUCCESS);

    ASSERT_EQ(device.submissions.size(), 1u);
    EXPECT_EQ(device.submissions[0].waits, ours);
    EXPECT_EQ(device.submissions[0].wait_stages, std::vector<VkPipelineStageFlags>(2, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));
    EXPECT_EQ(device.submissions[0].command_buffer_count, 0u);
    EXPECT_TRUE(device.submissions[0].signals.empty());
}

}  // namespace
