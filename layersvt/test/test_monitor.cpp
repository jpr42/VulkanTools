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
#include <thread>

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
