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

#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <vulkan/utility/vk_dispatch_table.h>

#include "monitor_overlay.h"
#include "monitor_renderer.h"

namespace monitor {

// Per swapchain image objects. The semaphore is what vkQueuePresentKHR waits on for this image; reusing it per image
// index is safe because the image can only be acquired again after the previous present of it has started.
struct SwapchainImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkSemaphore present_wait = VK_NULL_HANDLE;
    bool created = false;  // View and framebuffer exist
};

struct SwapchainOverlay {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent = {};
    VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    bool deferred = false;  // Image memory is bound on first acquire, so views are made at the first present
    bool blend = true;
    bool ok = false;               // The overlay can be drawn
    const char *reason = nullptr;  // Why not, when !ok
    OverlayPipeline pipeline;
    std::vector<SwapchainImage> images;
    FrameStats stats;
};

// Sets up everything needed to draw on an already created swapchain. On failure `overlay.ok` is false, the swapchain
// still works, and DestroySwapchainOverlay is still the way to clean up.
void SetupSwapchainOverlay(VkDevice device, const VkuDeviceDispatchTable &dispatch, VkSwapchainKHR swapchain,
                           const VkSwapchainCreateInfoKHR &create_info, bool blend_supported, SwapchainOverlay &overlay);

// Makes sure the image view and framebuffer for an acquired image exist. Returns false if they couldn't be created.
bool PrepareImage(VkDevice device, const VkuDeviceDispatchTable &dispatch, SwapchainOverlay &overlay, uint32_t index);

// The swapchain must not be in use by the GPU (see WaitForSwapchain).
void DestroySwapchainOverlay(VkDevice device, const VkuDeviceDispatchTable &dispatch, SwapchainOverlay &overlay);

// One overlay submission: a command buffer and the fence that says when it's done.
struct SubmitSlot {
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool in_flight = false;                  // Submitted, so the fence will signal
    std::vector<VkSwapchainKHR> swapchains;  // Drawn on by that submission
};

// Submission slots for one queue family, used round robin. Externally synchronized.
struct SlotRing {
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<SubmitSlot> slots;
    uint32_t next = 0;
    bool ok = false;
    const char *reason = nullptr;  // Why not, when !ok
};

// The layer makes its own command buffers, so the loader needs `set_loader_data` to give them a dispatch pointer.
// Without it the ring isn't usable.
void CreateSlotRing(VkDevice device, const VkuDeviceDispatchTable &dispatch, uint32_t queue_family,
                    PFN_vkSetDeviceLoaderData set_loader_data, uint32_t slot_count, SlotRing &ring);

// Returns the next slot once its previous submission is done, ready to record.
SubmitSlot &AcquireSlot(VkDevice device, const VkuDeviceDispatchTable &dispatch, SlotRing &ring);

// Submits the slot's recorded command buffer. The app's semaphores are waited on before the overlay draws, and the
// signal semaphores are what the present then waits on. On failure the slot is left reusable.
VkResult SubmitOverlay(VkDevice device, const VkuDeviceDispatchTable &dispatch, VkQueue queue, SubmitSlot &slot,
                       const std::vector<VkSemaphore> &waits, const std::vector<VkSemaphore> &signals,
                       const std::vector<VkSwapchainKHR> &swapchains);

// Waits until no submission is drawing on the swapchain.
void WaitForSwapchain(VkDevice device, const VkuDeviceDispatchTable &dispatch, SlotRing &ring, VkSwapchainKHR swapchain);

void DestroySlotRing(VkDevice device, const VkuDeviceDispatchTable &dispatch, SlotRing &ring);

// Storage for a VkPresentInfoKHR rewritten to wait on the overlay, and the parts of its pNext chain that were copied.
// The rewritten info points into this struct, so it must stay in place until the present is done. Reusing one per
// device keeps presents from allocating.
struct PresentRewrite {
    VkPresentInfoKHR info = {};
    std::vector<VkSemaphore> waits;
    VkPresentRegionsKHR present_regions = {};
    std::vector<VkPresentRegionKHR> regions;
    std::vector<std::vector<uint64_t>> copies;
    bool regions_unchanged = false;  // VkPresentRegionsKHR was behind a struct that can't be copied
};

// The present waits on `waits` instead of the app's semaphores. Swapchains with `overlay[i]` set report their whole
// image as changed in VkPresentRegionsKHR, so incremental presents still update the overlay. The rest of the pNext
// chain is passed through.
const VkPresentInfoKHR &RewritePresentInfo(const VkPresentInfoKHR &original, const std::vector<VkSemaphore> &waits,
                                           const std::vector<bool> &overlay, PresentRewrite &rewrite);

// Waits on semaphores that were signaled for a present that then failed, so they can be signaled again.
VkResult DrainSemaphores(const VkuDeviceDispatchTable &dispatch, VkQueue queue, const std::vector<VkSemaphore> &semaphores);

}  // namespace monitor
