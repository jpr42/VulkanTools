/*
 * Copyright (c) 2023 The Khronos Group Inc.
 * Copyright (c) 2023 Valve Corporation
 * Copyright (c) 2023 LunarG, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Author: Christophe Riccio <christophe@lunarg.com>
 */

#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_beta.h>

#include <gtest/gtest.h>
#include "layer_test_helper.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static const char* kLayerName = "VK_LAYER_LUNARG_monitor";

class MonitorTests : public VkTestFramework {
   public:
    ~MonitorTests(){};

    static void SetUpTestSuite() {}
    static void TearDownTestSuite(){};
};

TEST_F(MonitorTests, init_layer) {
    TEST_DESCRIPTION("Test Creating a Vulkan Instance with a layer");

    layer_test::VulkanInstanceBuilder inst_builder;
    VkResult err = inst_builder.Init(kLayerName);
    EXPECT_EQ(err, VK_SUCCESS);
}

static VkResult CreateDevice(VkPhysicalDevice physical_device, VkDevice* device) {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = 0;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    return vkCreateDevice(physical_device, &device_info, nullptr, device);
}

TEST_F(MonitorTests, CreateDevicesConcurrently) {
    TEST_DESCRIPTION("Create and destroy devices from two threads at once");

    layer_test::VulkanInstanceBuilder inst_builder;
    ASSERT_EQ(inst_builder.Init(kLayerName), VK_SUCCESS);
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    ASSERT_EQ(inst_builder.GetPhysicalDevice(&physical_device), VK_SUCCESS);

    std::atomic<int> failures{0};
    std::atomic<int> ready{0};
    auto create_and_destroy = [&] {
        // Start both threads together so their calls overlap
        ++ready;
        while (ready < 2) {
            std::this_thread::yield();
        }
        for (int i = 0; i < 25; ++i) {
            VkDevice device = VK_NULL_HANDLE;
            if (CreateDevice(physical_device, &device) != VK_SUCCESS) {
                ++failures;
                continue;
            }
            vkDestroyDevice(device, nullptr);
        }
    };
    std::thread first_thread(create_and_destroy);
    std::thread second_thread(create_and_destroy);
    first_thread.join();
    second_thread.join();
    EXPECT_EQ(failures, 0);
}

TEST_F(MonitorTests, CreateDeviceAfterRecreatingInstance) {
    TEST_DESCRIPTION("A new instance can reuse the handles of a destroyed one");

    for (int i = 0; i < 3; ++i) {
        layer_test::VulkanInstanceBuilder inst_builder;
        ASSERT_EQ(inst_builder.Init(kLayerName), VK_SUCCESS);
        VkPhysicalDevice physical_device = VK_NULL_HANDLE;
        ASSERT_EQ(inst_builder.GetPhysicalDevice(&physical_device), VK_SUCCESS);

        VkDevice device = VK_NULL_HANDLE;
        ASSERT_EQ(CreateDevice(physical_device, &device), VK_SUCCESS);
        vkDestroyDevice(device, nullptr);
    }
}

namespace {

// An instance, device and headless surface for swapchain tests. Skips if the driver has no headless surfaces.
class MonitorSwapchainTests : public VkTestFramework {
   public:
    static void SetUpTestSuite() {}
    static void TearDownTestSuite() {}

   protected:
    void SetUp() override {
        uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> extensions(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
        bool headless = false;
        bool display = false;
        for (const VkExtensionProperties& extension : extensions) {
            headless |= strcmp(extension.extensionName, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME) == 0;
            display |= strcmp(extension.extensionName, VK_KHR_DISPLAY_EXTENSION_NAME) == 0;
        }
        if (!headless) {
            GTEST_SKIP() << "VK_EXT_headless_surface is not supported";
        }

        inst_builder.AddExtension(VK_KHR_SURFACE_EXTENSION_NAME);
        inst_builder.AddExtension(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
        if (display) {
            inst_builder.AddExtension(VK_KHR_DISPLAY_EXTENSION_NAME);
        }
        ASSERT_EQ(inst_builder.Init(kLayerName), VK_SUCCESS);
        ASSERT_EQ(inst_builder.GetPhysicalDevice(&physical_device), VK_SUCCESS);

        // These tests check the layer's bookkeeping, and rely on the mock driver accepting any swapchain
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physical_device, &properties);
        if (strcmp(properties.deviceName, "Vulkan Mock Device") != 0) {
            GTEST_SKIP() << "needs the Vulkan-Tools mock driver";
        }

        // vkCreateSharedSwapchainsKHR comes from VK_KHR_display_swapchain
        std::vector<const char*> device_extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        if (display && layer_test::IsExtensionSupported(physical_device, VK_KHR_DISPLAY_SWAPCHAIN_EXTENSION_NAME)) {
            device_extensions.push_back(VK_KHR_DISPLAY_SWAPCHAIN_EXTENSION_NAME);
        }

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = 0;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
        device_info.ppEnabledExtensionNames = device_extensions.data();
        ASSERT_EQ(vkCreateDevice(physical_device, &device_info, nullptr, &device), VK_SUCCESS);
        vkGetDeviceQueue(device, 0, 0, &queue);

        auto create_surface = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
            vkGetInstanceProcAddr(inst_builder.GetInstance(), "vkCreateHeadlessSurfaceEXT"));
        const VkHeadlessSurfaceCreateInfoEXT surface_info = {VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
        ASSERT_EQ(create_surface(inst_builder.GetInstance(), &surface_info, nullptr, &surface), VK_SUCCESS);

        const VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        ASSERT_EQ(vkCreateFence(device, &fence_info, nullptr, &acquired), VK_SUCCESS);
    }

    void TearDown() override {
        if (device) {
            vkDeviceWaitIdle(device);
            vkDestroyFence(device, acquired, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (surface) {
            vkDestroySurfaceKHR(inst_builder.GetInstance(), surface, nullptr);
        }
    }

    void DestroySwapchain(VkSwapchainKHR swapchain) const {
        vkDeviceWaitIdle(device);
        vkDestroySwapchainKHR(device, swapchain, nullptr);
    }

    VkSwapchainCreateInfoKHR SwapchainInfo(VkSwapchainKHR old_swapchain = VK_NULL_HANDLE) const {
        VkSwapchainCreateInfoKHR info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface;
        info.minImageCount = 2;
        info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
        info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        info.imageExtent = {640, 480};
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        info.oldSwapchain = old_swapchain;
        return info;
    }

    VkResult Present(VkSwapchainKHR swapchain) {
        uint32_t index = 0;
        VkResult result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, VK_NULL_HANDLE, acquired, &index);
        if (result != VK_SUCCESS) return result;
        vkWaitForFences(device, 1, &acquired, VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &acquired);
        VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &index;
        return vkQueuePresentKHR(queue, &present);
    }

    layer_test::VulkanInstanceBuilder inst_builder;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkFence acquired = VK_NULL_HANDLE;
};
}  // namespace

TEST_F(MonitorSwapchainTests, PresentRecreateAndDestroy) {
    TEST_DESCRIPTION("Present, recreate the swapchain from the old one, and tear everything down");

    testing::internal::CaptureStderr();
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkSwapchainCreateInfoKHR info = SwapchainInfo();
    ASSERT_EQ(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain), VK_SUCCESS);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(Present(swapchain), VK_SUCCESS);
    }

    VkSwapchainKHR recreated = VK_NULL_HANDLE;
    info = SwapchainInfo(swapchain);
    ASSERT_EQ(vkCreateSwapchainKHR(device, &info, nullptr, &recreated), VK_SUCCESS);
    DestroySwapchain(swapchain);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(Present(recreated), VK_SUCCESS);
    }
    DestroySwapchain(recreated);
    EXPECT_EQ(testing::internal::GetCapturedStderr(), "");
}

TEST_F(MonitorSwapchainTests, CopyOnlySwapchainCanStillHaveTheOverlay) {
    TEST_DESCRIPTION("A swapchain the app only copies into gets the overlay too");

    testing::internal::CaptureStderr();
    VkSwapchainCreateInfoKHR info = SwapchainInfo();
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    ASSERT_EQ(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain), VK_SUCCESS);
    EXPECT_EQ(Present(swapchain), VK_SUCCESS);
    DestroySwapchain(swapchain);
    EXPECT_EQ(testing::internal::GetCapturedStderr(), "");
}

TEST_F(MonitorSwapchainTests, DestroyingOneSwapchainKeepsTheOther) {
    TEST_DESCRIPTION("Each swapchain is tracked on its own, like windows created and destroyed at runtime");

    const VkSwapchainCreateInfoKHR info = SwapchainInfo();
    VkSwapchainKHR first = VK_NULL_HANDLE;
    VkSwapchainKHR second = VK_NULL_HANDLE;
    ASSERT_EQ(vkCreateSwapchainKHR(device, &info, nullptr, &first), VK_SUCCESS);
    ASSERT_EQ(vkCreateSwapchainKHR(device, &info, nullptr, &second), VK_SUCCESS);
    DestroySwapchain(first);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(Present(second), VK_SUCCESS);
    }
    DestroySwapchain(second);
}

TEST_F(MonitorSwapchainTests, UnsupportedSwapchainsWarnOnce) {
    TEST_DESCRIPTION("Swapchains the overlay can't draw on still work, and say why once");

    struct Case {
        VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
        VkSwapchainCreateFlagsKHR flags = 0;
        const char* reason = nullptr;
    };
    const Case cases[] = {
        {VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR, 0, "shared present modes"},
        {VK_PRESENT_MODE_FIFO_KHR, VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR, "protected swapchains"},
    };
    for (const Case& test_case : cases) {
        testing::internal::CaptureStderr();
        for (int i = 0; i < 2; ++i) {
            VkSwapchainCreateInfoKHR info = SwapchainInfo();
            info.presentMode = test_case.present_mode;
            info.flags = test_case.flags;
            VkSwapchainKHR swapchain = VK_NULL_HANDLE;
            ASSERT_EQ(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain), VK_SUCCESS);
            DestroySwapchain(swapchain);
        }
        const std::string output = testing::internal::GetCapturedStderr();
        const size_t first = output.find(test_case.reason);
        EXPECT_NE(first, std::string::npos) << output;
        EXPECT_EQ(output.find(test_case.reason, first + 1), std::string::npos) << output;
    }
}

TEST_F(MonitorSwapchainTests, SharedSwapchainsWarnOnce) {
    TEST_DESCRIPTION("vkCreateSharedSwapchainsKHR swapchains still work, with no overlay");

    testing::internal::CaptureStderr();
    const VkSwapchainCreateInfoKHR info = SwapchainInfo();
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    auto create_shared =
        reinterpret_cast<PFN_vkCreateSharedSwapchainsKHR>(vkGetDeviceProcAddr(device, "vkCreateSharedSwapchainsKHR"));
    if (!create_shared) {
        testing::internal::GetCapturedStderr();
        GTEST_SKIP() << "vkCreateSharedSwapchainsKHR is not available";
    }
    ASSERT_EQ(create_shared(device, 1, &info, nullptr, &swapchain), VK_SUCCESS);
    DestroySwapchain(swapchain);
    EXPECT_NE(testing::internal::GetCapturedStderr().find("vkCreateSharedSwapchainsKHR"), std::string::npos);
}
