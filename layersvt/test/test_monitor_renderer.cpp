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

// Renders the overlay into an offscreen image and compares it with a CPU reference.
// Needs a real Vulkan driver; skips otherwise.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "monitor_overlay.h"
#include "monitor_renderer.h"

namespace {

const float kClear[4] = {0.25f, 0.5f, 0.75f, 0.5f};
const VkExtent2D kExtent = {320, 200};
const std::vector<std::string> kLines = {"FPS 143.2", "0123456789.-()", "FPSaemrs Frame"};

struct Gpu {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkuDeviceDispatchTable dispatch = {};
    std::string skip_reason;
};

uint32_t validation_errors = 0;
std::string last_validation_error;

VKAPI_ATTR VkBool32 VKAPI_CALL OnValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                   VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                                   const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*pUserData*/) {
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        ++validation_errors;
        last_validation_error = data->pMessage;
    }
    return VK_FALSE;
}

bool HasInstanceLayer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const VkLayerProperties& layer : layers) {
        if (strcmp(layer.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

bool HasDeviceExtension(VkPhysicalDevice physical_device, const char* name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, extensions.data());
    for (const VkExtensionProperties& extension : extensions) {
        if (strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

// Validation is used when installed, to catch mistakes in the overlay's own Vulkan usage.
void CreateGpu(Gpu& gpu) {
    const bool validation = HasInstanceLayer("VK_LAYER_KHRONOS_validation");
    const char* const layers[] = {"VK_LAYER_KHRONOS_validation"};
    std::vector<const char*> instance_extensions = {VK_KHR_SURFACE_EXTENSION_NAME};
    if (validation) {
        instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "test_monitor_renderer";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    instance_info.enabledLayerCount = validation ? 1 : 0;
    instance_info.ppEnabledLayerNames = layers;
    instance_info.enabledExtensionCount = static_cast<uint32_t>(instance_extensions.size());
    instance_info.ppEnabledExtensionNames = instance_extensions.data();
    if (vkCreateInstance(&instance_info, nullptr, &gpu.instance) != VK_SUCCESS) {
        gpu.skip_reason = "no Vulkan driver";
        return;
    }

    if (validation) {
        VkDebugUtilsMessengerCreateInfoEXT messenger_info = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        messenger_info.pfnUserCallback = OnValidationMessage;
        auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(gpu.instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create_messenger) {
            create_messenger(gpu.instance, &messenger_info, nullptr, &gpu.messenger);
        }
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(gpu.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physical_devices(count);
    vkEnumeratePhysicalDevices(gpu.instance, &count, physical_devices.data());

    uint32_t family = UINT32_MAX;
    for (VkPhysicalDevice physical_device : physical_devices) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physical_device, &properties);
        if (strcmp(properties.deviceName, "Vulkan Mock Device") == 0) {
            continue;
        }
        if (!HasDeviceExtension(physical_device, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            continue;
        }

        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families.data());
        for (uint32_t i = 0; i < family_count; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u) {
                gpu.physical_device = physical_device;
                family = i;
                break;
            }
        }
        if (gpu.physical_device) {
            break;
        }
    }
    if (!gpu.physical_device) {
        gpu.skip_reason = "no physical device with graphics and VK_KHR_swapchain";
        return;
    }

    // VK_KHR_swapchain is needed for the PRESENT_SRC_KHR layout the overlay renders from and to.
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    const char* const device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = device_extensions;
    if (vkCreateDevice(gpu.physical_device, &device_info, nullptr, &gpu.device) != VK_SUCCESS) {
        gpu.skip_reason = "vkCreateDevice failed";
        return;
    }
    vkuInitDeviceDispatchTable(gpu.device, &gpu.dispatch, vkGetDeviceProcAddr);
    vkGetDeviceQueue(gpu.device, family, 0, &gpu.queue);

    VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = family;
    vkCreateCommandPool(gpu.device, &pool_info, nullptr, &gpu.pool);
}

void DestroyGpu(Gpu& gpu) {
    if (gpu.device) {
        vkDestroyCommandPool(gpu.device, gpu.pool, nullptr);
        vkDestroyDevice(gpu.device, nullptr);
    }
    if (gpu.messenger) {
        auto destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(gpu.instance, "vkDestroyDebugUtilsMessengerEXT"));
        destroy_messenger(gpu.instance, gpu.messenger, nullptr);
    }
    if (gpu.instance) {
        vkDestroyInstance(gpu.instance, nullptr);
    }
    gpu = {};
}

uint32_t FindMemoryType(VkPhysicalDevice physical_device, uint32_t type_bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if (((type_bits & (1u << i)) != 0u) && (memory.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    return UINT32_MAX;
}

float SrgbToLinear(float value) { return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f); }
float LinearToSrgb(float value) { return value <= 0.0031308f ? value * 12.92f : (1.055f * std::pow(value, 1.0f / 2.4f)) - 0.055f; }

float HalfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1;
    const uint32_t exponent = (h >> 10) & 0x1F;
    const uint32_t mantissa = h & 0x3FF;
    float value = NAN;
    if (exponent == 0) {
        value = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 31) {
        value = (mantissa != 0u) ? NAN : INFINITY;
    } else {
        value = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
    }
    return (sign != 0u) ? -value : value;
}

enum class Pixel : uint8_t { Outside, Panel, Text };

// What the overlay should draw at the center of image pixel (x, y), from the same layout and font as the shaders.
Pixel ReferencePixel(const monitor::OverlayLayout& layout, const std::vector<std::string>& lines, uint32_t x, uint32_t y) {
    const float ix = static_cast<float>(x) + 0.5f;
    const float iy = static_cast<float>(y) + 0.5f;
    const auto vw = static_cast<float>(layout.visible_extent.width);
    const auto vh = static_cast<float>(layout.visible_extent.height);
    float vx = ix;
    float vy = iy;
    switch (layout.rotation) {
        case 1:
            vx = iy;
            vy = vh - ix;
            break;
        case 2:
            vx = vw - ix;
            vy = vh - iy;
            break;
        case 3:
            vx = vw - iy;
            vy = ix;
            break;
        default:
            break;
    }
    const auto left = static_cast<float>(layout.visible_rect[0]);
    const auto top = static_cast<float>(layout.visible_rect[1]);
    if (vx < left || vx >= static_cast<float>(layout.visible_rect[2]) || vy < top ||
        vy >= static_cast<float>(layout.visible_rect[3])) {
        return Pixel::Outside;
    }

    const auto px = static_cast<float>(layout.pixel_size);
    const int font_x = static_cast<int>(std::floor((vx - left) / px)) - monitor::kPadding;
    const int font_y = static_cast<int>(std::floor((vy - top) / px)) - monitor::kPadding;
    if (font_x < 0 || font_y < 0) {
        return Pixel::Panel;
    }
    const auto line = static_cast<size_t>(font_y / monitor::kLineAdvance);
    const auto column = static_cast<size_t>(font_x / monitor::kCharAdvance);
    const int glyph_x = font_x % monitor::kCharAdvance;
    const int glyph_y = font_y % monitor::kLineAdvance;
    if (line >= lines.size() || column >= lines[line].size() || glyph_x >= monitor::kGlyphWidth ||
        glyph_y >= monitor::kGlyphHeight) {
        return Pixel::Panel;
    }
    const uint8_t row = monitor::GlyphRow(monitor::GlyphIndex(lines[line][column]), glyph_y);
    return (((row >> (monitor::kGlyphWidth - 1 - glyph_x)) & 1) != 0) ? Pixel::Text : Pixel::Panel;
}

// One color channel of the expected image, given the cleared value underneath
float ExpectedChannel(Pixel kind, float clear, bool blend) {
    switch (kind) {
        case Pixel::Text:
            return 1.0f;
        case Pixel::Panel:
            return blend ? clear * 0.4f : 0.0f;
        case Pixel::Outside:
        default:
            return clear;
    }
}

// How a test image format stores color
struct TexelFormat {
    bool srgb = false;
    bool half = false;
    bool bgr = false;
};

TexelFormat TexelFormatOf(VkFormat format) {
    TexelFormat texel_format;
    texel_format.srgb = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB;
    texel_format.half = format == VK_FORMAT_R16G16B16A16_SFLOAT;
    texel_format.bgr = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
    return texel_format;
}

using Color = std::array<float, 4>;

// Reads one texel back as linear RGBA
Color ReadTexel(const uint8_t* data, const TexelFormat& format) {
    Color color = {};
    for (size_t i = 0; i < color.size(); ++i) {
        if (format.half) {
            uint16_t bits = 0;
            memcpy(&bits, data + (i * 2), 2);
            color[i] = HalfToFloat(bits);
        } else {
            color[i] = static_cast<float>(data[i]) / 255.0f;
            if (format.srgb && i < 3) {
                color[i] = SrgbToLinear(color[i]);
            }
        }
    }
    if (format.bgr) {
        std::swap(color[0], color[2]);
    }
    return color;
}

// The clear color as stored, read back in linear space
float StoredClear(float linear, const TexelFormat& format) {
    return format.srgb ? SrgbToLinear(std::round(LinearToSrgb(linear) * 255) / 255) : linear;
}

Color ExpectedColor(Pixel kind, const TexelFormat& format, bool blend) {
    Color color = {};
    for (size_t i = 0; i < 3; ++i) {
        color[i] = ExpectedChannel(kind, StoredClear(kClear[i], format), blend);
    }
    color[3] = kClear[3];  // The overlay never writes alpha
    return color;
}

// sRGB tolerance is in encoded space, so compare there
bool ColorsMatch(const Color& actual, const Color& expected, const TexelFormat& format) {
    const float tolerance = format.half ? 0.002f : 1.5f / 255.0f;
    for (size_t i = 0; i < actual.size(); ++i) {
        const bool encoded = format.srgb && i < 3;
        const float actual_value = encoded ? LinearToSrgb(actual[i]) : actual[i];
        const float expected_value = encoded ? LinearToSrgb(expected[i]) : expected[i];
        if (std::fabs(actual_value - expected_value) > tolerance) {
            return false;
        }
    }
    return true;
}

std::string ColorString(const Color& color) {
    std::ostringstream out;
    out << "(" << color[0] << ", " << color[1] << ", " << color[2] << ", " << color[3] << ")";
    return out.str();
}

struct Case {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    bool blend = false;
    bool oversized_panel = false;  // Pass RecordOverlay a panel that spills past every edge of the image
};

std::string CaseName(const Case& test_case) {
    return "format " + std::to_string(test_case.format) + " transform " + std::to_string(test_case.transform) +
           (test_case.blend ? " blended" : " opaque");
}

class MonitorRenderer : public ::testing::Test {
   protected:
    static void SetUpTestSuite() { CreateGpu(gpu_); }
    static void TearDownTestSuite() { DestroyGpu(gpu_); }

    void SetUp() override {
        if (!gpu_.skip_reason.empty()) {
            GTEST_SKIP() << gpu_.skip_reason;
        }
        validation_errors = 0;
    }

    void TearDown() override { EXPECT_EQ(validation_errors, 0u) << last_validation_error; }

    // Clears an image, draws the overlay into it while it is in PRESENT_SRC_KHR, then checks every pixel.
    static void Check(const Case& test_case) {
        SCOPED_TRACE(CaseName(test_case));
        VkFormatProperties format_properties;
        vkGetPhysicalDeviceFormatProperties(gpu_.physical_device, test_case.format, &format_properties);
        VkFormatFeatureFlags needed =
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if (test_case.blend) {
            needed |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
        }
        if ((format_properties.optimalTilingFeatures & needed) != needed) {
            std::cout << "[ SKIPPED  ] " << CaseName(test_case) << ": format features missing\n";
            return;
        }

        const VkDevice device = gpu_.device;
        const auto& dispatch = gpu_.dispatch;
        const uint32_t texel = test_case.format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;

        VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = test_case.format;
        image_info.extent = {kExtent.width, kExtent.height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImage image = nullptr;
        ASSERT_EQ(dispatch.CreateImage(device, &image_info, nullptr, &image), VK_SUCCESS);
        VkMemoryRequirements image_requirements;
        dispatch.GetImageMemoryRequirements(device, image, &image_requirements);
        VkMemoryAllocateInfo allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = image_requirements.size;
        allocate.memoryTypeIndex =
            FindMemoryType(gpu_.physical_device, image_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDeviceMemory image_memory = nullptr;
        ASSERT_EQ(dispatch.AllocateMemory(device, &allocate, nullptr, &image_memory), VK_SUCCESS);
        dispatch.BindImageMemory(device, image, image_memory, 0);

        VkBufferCreateInfo buffer_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = static_cast<VkDeviceSize>(kExtent.width) * kExtent.height * texel;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer buffer = nullptr;
        ASSERT_EQ(dispatch.CreateBuffer(device, &buffer_info, nullptr, &buffer), VK_SUCCESS);
        VkMemoryRequirements buffer_requirements;
        dispatch.GetBufferMemoryRequirements(device, buffer, &buffer_requirements);
        allocate.allocationSize = buffer_requirements.size;
        allocate.memoryTypeIndex = FindMemoryType(gpu_.physical_device, buffer_requirements.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory buffer_memory = nullptr;
        ASSERT_EQ(dispatch.AllocateMemory(device, &allocate, nullptr, &buffer_memory), VK_SUCCESS);
        dispatch.BindBufferMemory(device, buffer, buffer_memory, 0);

        VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = test_case.format;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = nullptr;
        ASSERT_EQ(dispatch.CreateImageView(device, &view_info, nullptr, &view), VK_SUCCESS);

        monitor::OverlayPipeline pipeline;
        ASSERT_EQ(monitor::CreateOverlayPipeline(device, dispatch, test_case.format, test_case.blend, pipeline), VK_SUCCESS);

        VkFramebufferCreateInfo framebuffer_info = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer_info.renderPass = pipeline.render_pass;
        framebuffer_info.attachmentCount = 1;
        framebuffer_info.pAttachments = &view;
        framebuffer_info.width = kExtent.width;
        framebuffer_info.height = kExtent.height;
        framebuffer_info.layers = 1;
        VkFramebuffer framebuffer = nullptr;
        ASSERT_EQ(dispatch.CreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer), VK_SUCCESS);

        const monitor::OverlayLayout layout =
            monitor::ComputeOverlayLayout(kExtent, monitor::Position::TopLeft, 100, test_case.transform, kLines);
        const monitor::OverlayPushConstants push_constants =
            monitor::PackOverlayPushConstants(layout, kLines, 1.0f, !test_case.blend);

        VkCommandBufferAllocateInfo command_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command_info.commandPool = gpu_.pool;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        VkCommandBuffer cmd = nullptr;
        ASSERT_EQ(dispatch.AllocateCommandBuffers(device, &command_info, &cmd), VK_SUCCESS);
        const VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        dispatch.BeginCommandBuffer(cmd, &begin);

        VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = view_info.subresourceRange;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dispatch.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                    nullptr, 1, &barrier);
        VkClearColorValue clear;
        memcpy(clear.float32, kClear, sizeof(kClear));
        dispatch.CmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &view_info.subresourceRange);

        // Hand the image over the way an app hands a swapchain image to vkQueuePresentKHR.
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        dispatch.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                                    nullptr, 1, &barrier);

        const int32_t oversized[4] = {-16, -16, static_cast<int32_t>(kExtent.width) + 16,
                                      static_cast<int32_t>(kExtent.height) + 16};
        monitor::RecordOverlay(dispatch, cmd, pipeline, framebuffer, kExtent,
                               test_case.oversized_panel ? oversized : layout.image_rect, push_constants);

        barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        dispatch.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                    nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy copy = {};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {kExtent.width, kExtent.height, 1};
        dispatch.CmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        VkBufferMemoryBarrier buffer_barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        buffer_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        buffer_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        buffer_barrier.buffer = buffer;
        buffer_barrier.size = VK_WHOLE_SIZE;
        dispatch.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                                    &buffer_barrier, 0, nullptr);
        dispatch.EndCommandBuffer(cmd);

        VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        ASSERT_EQ(dispatch.QueueSubmit(gpu_.queue, 1, &submit, VK_NULL_HANDLE), VK_SUCCESS);
        ASSERT_EQ(dispatch.QueueWaitIdle(gpu_.queue), VK_SUCCESS);

        void* mapped = nullptr;
        ASSERT_EQ(dispatch.MapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped), VK_SUCCESS);
        ASSERT_NE(mapped, nullptr);
        CompareWithReference(test_case, layout, static_cast<const uint8_t*>(mapped), texel);
        dispatch.UnmapMemory(device, buffer_memory);

        dispatch.FreeCommandBuffers(device, gpu_.pool, 1, &cmd);
        dispatch.DestroyFramebuffer(device, framebuffer, nullptr);
        monitor::DestroyOverlayPipeline(device, dispatch, pipeline);
        dispatch.DestroyImageView(device, view, nullptr);
        dispatch.DestroyBuffer(device, buffer, nullptr);
        dispatch.FreeMemory(device, buffer_memory, nullptr);
        dispatch.DestroyImage(device, image, nullptr);
        dispatch.FreeMemory(device, image_memory, nullptr);
    }

    // Text and the panel outline must match exactly; blended colors get a small tolerance.
    static void CompareWithReference(const Case& test_case, const monitor::OverlayLayout& layout, const uint8_t* pixels,
                                     uint32_t texel) {
        const TexelFormat format = TexelFormatOf(test_case.format);
        uint32_t mismatches = 0;
        uint32_t text_pixels = 0;
        for (uint32_t y = 0; y < kExtent.height; ++y) {
            for (uint32_t x = 0; x < kExtent.width; ++x) {
                const Color actual = ReadTexel(pixels + (((static_cast<size_t>(y) * kExtent.width) + x) * texel), format);
                const Pixel kind = ReferencePixel(layout, kLines, x, y);
                const Color expected = ExpectedColor(kind, format, test_case.blend);
                text_pixels += kind == Pixel::Text ? 1 : 0;
                if (!ColorsMatch(actual, expected, format) && ++mismatches <= 5) {
                    ADD_FAILURE() << "pixel (" << x << ", " << y << ") is " << ColorString(actual) << ", expected "
                                  << ColorString(expected);
                }
            }
        }
        EXPECT_EQ(mismatches, 0u);
        EXPECT_GT(text_pixels, 0u);
    }

    static Gpu gpu_;
};

Gpu MonitorRenderer::gpu_;

TEST_F(MonitorRenderer, DrawsOverlay) { Check({VK_FORMAT_R8G8B8A8_UNORM, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, true}); }

TEST_F(MonitorRenderer, DrawsOverlayUprightForEachRotation) {
    for (const VkSurfaceTransformFlagBitsKHR transform :
         {VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR, VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR,
          VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR}) {
        Check({VK_FORMAT_R8G8B8A8_UNORM, transform, true});
    }
}

TEST_F(MonitorRenderer, DrawsOpaquePanelWithoutBlending) {
    Check({VK_FORMAT_R8G8B8A8_UNORM, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, false});
}

TEST_F(MonitorRenderer, DrawsOverlayInSrgbAndFloatFormats) {
    Check({VK_FORMAT_B8G8R8A8_SRGB, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, true});
    Check({VK_FORMAT_R16G16B16A16_SFLOAT, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, true});
}

TEST_F(MonitorRenderer, KeepsRenderAreaInsideTheImage) {
    Check({VK_FORMAT_R8G8B8A8_UNORM, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, true, true});
}

}  // namespace
