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

#include "monitor_present.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "monitor_overlay.h"
#include "monitor_renderer.h"

namespace monitor {

void SetupSwapchainOverlay(VkDevice device, const VkuDeviceDispatchTable &dispatch, VkSwapchainKHR swapchain,
                           const VkSwapchainCreateInfoKHR &create_info, bool blend_supported, SwapchainOverlay &overlay) {
    overlay = {};
    overlay.swapchain = swapchain;
    overlay.format = create_info.imageFormat;
    overlay.color_space = create_info.imageColorSpace;
    overlay.extent = create_info.imageExtent;
    overlay.transform = create_info.preTransform;
    overlay.blend = blend_supported;

    const Eligibility eligibility = SwapchainOverlayEligibility(create_info);
    overlay.deferred = eligibility.deferred;
    if (!eligibility.ok) {
        overlay.reason = eligibility.reason;
        return;
    }

    uint32_t count = 0;
    std::vector<VkImage> images;
    if (dispatch.GetSwapchainImagesKHR(device, swapchain, &count, nullptr) == VK_SUCCESS) {
        images.resize(count);
        if (dispatch.GetSwapchainImagesKHR(device, swapchain, &count, images.data()) != VK_SUCCESS) {
            images.clear();
        }
    }
    if (images.empty()) {
        overlay.reason = "could not get the swapchain images";
        return;
    }

    if (CreateOverlayPipeline(device, dispatch, overlay.format, overlay.blend, overlay.pipeline) != VK_SUCCESS) {
        overlay.reason = "could not create the overlay pipeline";
        return;
    }

    overlay.images.resize(images.size());
    for (size_t i = 0; i < images.size(); ++i) {
        overlay.images[i].image = images[i];
        const VkSemaphoreCreateInfo semaphore_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (dispatch.CreateSemaphore(device, &semaphore_info, nullptr, &overlay.images[i].present_wait) != VK_SUCCESS) {
            overlay.reason = "could not create semaphores";
            return;
        }
    }

    // Deferred images have no memory until acquired, so their views wait for the first present
    if (!overlay.deferred) {
        for (uint32_t i = 0; i < overlay.images.size(); ++i) {
            if (!PrepareImage(device, dispatch, overlay, i)) {
                overlay.reason = "could not create image views";
                return;
            }
        }
    }
    overlay.ok = true;
}

bool PrepareImage(VkDevice device, const VkuDeviceDispatchTable &dispatch, SwapchainOverlay &overlay, uint32_t index) {
    if (index >= overlay.images.size()) {
        return false;
    }
    SwapchainImage &image = overlay.images[index];
    if (image.created) {
        return true;
    }

    VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = image.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = overlay.format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (dispatch.CreateImageView(device, &view_info, nullptr, &image.view) != VK_SUCCESS) {
        image.view = VK_NULL_HANDLE;
        return false;
    }

    VkFramebufferCreateInfo framebuffer_info = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer_info.renderPass = overlay.pipeline.render_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &image.view;
    framebuffer_info.width = overlay.extent.width;
    framebuffer_info.height = overlay.extent.height;
    framebuffer_info.layers = 1;
    if (dispatch.CreateFramebuffer(device, &framebuffer_info, nullptr, &image.framebuffer) != VK_SUCCESS) {
        // Leave nothing behind, since a deferred image is tried again on its next present
        dispatch.DestroyImageView(device, image.view, nullptr);
        image.view = VK_NULL_HANDLE;
        image.framebuffer = VK_NULL_HANDLE;
        return false;
    }

    image.created = true;
    return true;
}

void DestroySwapchainOverlay(VkDevice device, const VkuDeviceDispatchTable &dispatch, SwapchainOverlay &overlay) {
    for (const SwapchainImage &image : overlay.images) {
        if (image.framebuffer) {
            dispatch.DestroyFramebuffer(device, image.framebuffer, nullptr);
        }
        if (image.view) {
            dispatch.DestroyImageView(device, image.view, nullptr);
        }
        if (image.present_wait) {
            dispatch.DestroySemaphore(device, image.present_wait, nullptr);
        }
    }
    DestroyOverlayPipeline(device, dispatch, overlay.pipeline);
    overlay = {};
}

void CreateSlotRing(VkDevice device, const VkuDeviceDispatchTable &dispatch, uint32_t queue_family,
                    PFN_vkSetDeviceLoaderData set_loader_data, uint32_t slot_count, SlotRing &ring) {
    ring = {};
    if (slot_count == 0) {
        ring.reason = "no submission slots";
        return;
    }
    if (!set_loader_data) {
        ring.reason = "the loader didn't provide vkSetDeviceLoaderData";
        return;
    }

    VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family;
    if (dispatch.CreateCommandPool(device, &pool_info, nullptr, &ring.pool) != VK_SUCCESS) {
        ring.pool = VK_NULL_HANDLE;
        ring.reason = "could not create a command pool";
        return;
    }

    ring.slots.resize(slot_count);
    for (SubmitSlot &slot : ring.slots) {
        VkCommandBufferAllocateInfo allocate_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate_info.commandPool = ring.pool;
        allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate_info.commandBufferCount = 1;
        if (dispatch.AllocateCommandBuffers(device, &allocate_info, &slot.command_buffer) != VK_SUCCESS) {
            slot.command_buffer = VK_NULL_HANDLE;
            ring.reason = "could not allocate command buffers";
            return;
        }
        if (set_loader_data(device, slot.command_buffer) != VK_SUCCESS) {
            ring.reason = "vkSetDeviceLoaderData failed";
            return;
        }

        const VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (dispatch.CreateFence(device, &fence_info, nullptr, &slot.fence) != VK_SUCCESS) {
            slot.fence = VK_NULL_HANDLE;
            ring.reason = "could not create fences";
            return;
        }
    }
    ring.ok = true;
}

static void WaitForSlot(VkDevice device, const VkuDeviceDispatchTable &dispatch, SubmitSlot &slot) {
    if (slot.in_flight) {
        dispatch.WaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
        slot.in_flight = false;
        slot.swapchains.clear();
    }
}

SubmitSlot &AcquireSlot(VkDevice device, const VkuDeviceDispatchTable &dispatch, SlotRing &ring) {
    SubmitSlot &slot = ring.slots[ring.next];
    ring.next = (ring.next + 1) % static_cast<uint32_t>(ring.slots.size());
    WaitForSlot(device, dispatch, slot);
    return slot;
}

VkResult SubmitOverlay(VkDevice device, const VkuDeviceDispatchTable &dispatch, VkQueue queue, SubmitSlot &slot,
                       const std::vector<VkSemaphore> &waits, const std::vector<VkSemaphore> &signals,
                       const std::vector<VkSwapchainKHR> &swapchains) {
    const std::vector<VkPipelineStageFlags> wait_stages(waits.size(), VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
    submit.pWaitSemaphores = waits.data();
    submit.pWaitDstStageMask = wait_stages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &slot.command_buffer;
    submit.signalSemaphoreCount = static_cast<uint32_t>(signals.size());
    submit.pSignalSemaphores = signals.data();

    // The fence is only reset when a submit will signal it again. A failed submit leaves it reset, but not in flight.
    VkResult result = dispatch.ResetFences(device, 1, &slot.fence);
    if (result != VK_SUCCESS) {
        return result;
    }
    result = dispatch.QueueSubmit(queue, 1, &submit, slot.fence);
    if (result == VK_SUCCESS) {
        slot.in_flight = true;
        slot.swapchains = swapchains;
    }
    return result;
}

void WaitForSwapchain(VkDevice device, const VkuDeviceDispatchTable &dispatch, SlotRing &ring, VkSwapchainKHR swapchain) {
    for (SubmitSlot &slot : ring.slots) {
        if (std::find(slot.swapchains.begin(), slot.swapchains.end(), swapchain) != slot.swapchains.end()) {
            WaitForSlot(device, dispatch, slot);
        }
    }
}

void DestroySlotRing(VkDevice device, const VkuDeviceDispatchTable &dispatch, SlotRing &ring) {
    for (SubmitSlot &slot : ring.slots) {
        WaitForSlot(device, dispatch, slot);
        if (slot.fence) {
            dispatch.DestroyFence(device, slot.fence, nullptr);
        }
        if (slot.command_buffer) {
            dispatch.FreeCommandBuffers(device, ring.pool, 1, &slot.command_buffer);
        }
    }
    if (ring.pool) {
        dispatch.DestroyCommandPool(device, ring.pool, nullptr);
    }
    ring = {};
}

// Sizes of the VkPresentInfoKHR extension structs that can come before VkPresentRegionsKHR. To change the regions,
// every struct ahead of them in the chain has to be copied, and only structs of known size can be.
static size_t PresentStructSize(VkStructureType type) {
    switch (type) {
        case VK_STRUCTURE_TYPE_DISPLAY_PRESENT_INFO_KHR:
            return sizeof(VkDisplayPresentInfoKHR);
        case VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR:
            return sizeof(VkDeviceGroupPresentInfoKHR);
        case VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE:
            return sizeof(VkPresentTimesInfoGOOGLE);
        case VK_STRUCTURE_TYPE_PRESENT_ID_KHR:
            return sizeof(VkPresentIdKHR);
        case VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR:
            return sizeof(VkPresentId2KHR);
        case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR:
            return sizeof(VkSwapchainPresentFenceInfoKHR);
        case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR:
            return sizeof(VkSwapchainPresentModeInfoKHR);
        case VK_STRUCTURE_TYPE_FRAME_BOUNDARY_EXT:
            return sizeof(VkFrameBoundaryEXT);
        default:
            return 0;
    }
}

// Rebuilds the start of the chain with a copy of VkPresentRegionsKHR in which overlay swapchains have no rectangles,
// meaning the whole image changed. Returns the original chain if there are no regions or they can't be reached.
static const void *RewritePresentRegions(const void *chain, const std::vector<bool> &overlay, PresentRewrite &rewrite) {
    std::vector<const VkBaseInStructure *> prefix;
    const auto *node = static_cast<const VkBaseInStructure *>(chain);
    for (; node && node->sType != VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR; node = node->pNext) {
        prefix.push_back(node);
    }
    if (!node) {
        return chain;
    }
    for (const VkBaseInStructure *before : prefix) {
        if (PresentStructSize(before->sType) == 0) {
            rewrite.regions_unchanged = true;
            return chain;
        }
    }

    rewrite.present_regions = *reinterpret_cast<const VkPresentRegionsKHR *>(node);
    if (rewrite.present_regions.pRegions) {
        rewrite.regions.assign(rewrite.present_regions.pRegions,
                               rewrite.present_regions.pRegions + rewrite.present_regions.swapchainCount);
        for (size_t i = 0; i < rewrite.regions.size() && i < overlay.size(); ++i) {
            if (overlay[i]) {
                rewrite.regions[i].rectangleCount = 0;
                rewrite.regions[i].pRectangles = nullptr;
            }
        }
        rewrite.present_regions.pRegions = rewrite.regions.data();
    }

    // Copy the structs ahead of the regions, back to front, each pointing at the next copy
    const void *next = &rewrite.present_regions;
    rewrite.copies.resize(prefix.size());
    for (size_t i = prefix.size(); i-- > 0;) {
        const size_t size = PresentStructSize(prefix[i]->sType);
        std::vector<uint64_t> &copy = rewrite.copies[i];
        copy.resize((size + sizeof(uint64_t) - 1) / sizeof(uint64_t));
        memcpy(copy.data(), prefix[i], size);
        reinterpret_cast<VkBaseInStructure *>(copy.data())->pNext = static_cast<const VkBaseInStructure *>(next);
        next = copy.data();
    }
    return next;
}

const VkPresentInfoKHR &RewritePresentInfo(const VkPresentInfoKHR &original, const std::vector<VkSemaphore> &waits,
                                           const std::vector<bool> &overlay, PresentRewrite &rewrite) {
    // The vectors keep their capacity, so presenting doesn't allocate once warmed up
    rewrite.waits.assign(waits.begin(), waits.end());
    rewrite.regions.clear();
    rewrite.regions_unchanged = false;
    rewrite.info = original;
    rewrite.info.waitSemaphoreCount = static_cast<uint32_t>(rewrite.waits.size());
    rewrite.info.pWaitSemaphores = rewrite.waits.data();
    rewrite.info.pNext = RewritePresentRegions(original.pNext, overlay, rewrite);
    return rewrite.info;
}

VkResult DrainSemaphores(const VkuDeviceDispatchTable &dispatch, VkQueue queue, const std::vector<VkSemaphore> &semaphores) {
    if (semaphores.empty()) {
        return VK_SUCCESS;
    }
    const std::vector<VkPipelineStageFlags> stages(semaphores.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = static_cast<uint32_t>(semaphores.size());
    submit.pWaitSemaphores = semaphores.data();
    submit.pWaitDstStageMask = stages.data();
    return dispatch.QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
}

}  // namespace monitor
