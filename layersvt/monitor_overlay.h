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

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace monitor {

struct StatsSnapshot {
    bool valid = false;
    double fps = 0.0;
    double avg_ms = 0.0;
    double min_ms = 0.0;
    double max_ms = 0.0;
};

// Present-to-present timing for one swapchain. The snapshot is refreshed once per update interval.
struct FrameStats {
    StatsSnapshot snapshot;

    bool started = false;
    std::chrono::steady_clock::time_point last_present;
    std::chrono::steady_clock::time_point interval_start;
    uint32_t frame_count = 0;
    double total_ms = 0.0;
    double min_ms = 0.0;
    double max_ms = 0.0;
};

void RecordPresent(FrameStats &stats, std::chrono::steady_clock::time_point now, std::chrono::milliseconds interval);

// Returns the number of the frame being presented. Frame 0 is the first presented image, which matches the
// frame number RenderDoc gives a capture of that frame.
uint64_t NextFrameNumber(std::atomic<uint64_t> &counter);

// 5x7 bitmap font. The fragment shader holds a copy of the same table.
constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;
constexpr int kGlyphCount = 23;

// Panel metrics in font pixels. The fragment shader holds a copy of these too.
constexpr int kCharAdvance = kGlyphWidth + 1;
constexpr int kLineAdvance = kGlyphHeight + 2;
constexpr int kPadding = 3;
constexpr int kMargin = 4;

// Characters the font can't draw map to the blank glyph.
uint8_t GlyphIndex(char character);

// Returns one row of a glyph, top row first. Bit 4 is the leftmost pixel.
uint8_t GlyphRow(uint8_t glyph, int row);

struct LineFlags {
    bool fps = true;
    bool frame_time = true;
    bool frame_number = true;
};

// Text lines of the overlay, top to bottom. Hidden lines are left out.
std::vector<std::string> FormatOverlayLines(const StatsSnapshot &snapshot, uint64_t frame_number, const LineFlags &flags);

enum class Position : uint8_t { TopLeft, TopRight, BottomLeft, BottomRight };

// Rectangles are {x0, y0, x1, y1}, x1 and y1 exclusive.
struct OverlayLayout {
    VkExtent2D visible_extent = {};  // Image extent as the user sees it, after preTransform
    int32_t visible_rect[4] = {};    // Panel as the user sees it
    int32_t image_rect[4] = {};      // Panel in swapchain image pixels
    uint32_t rotation = 0;           // Quarter turns clockwise from visible to image
    uint32_t pixel_size = 1;         // Image pixels per font pixel
    bool transform_warning = false;  // preTransform was mirrored or INHERIT and is drawn unmirrored
};

OverlayLayout ComputeOverlayLayout(VkExtent2D image_extent, Position position, int scale_percent,
                                   VkSurfaceTransformFlagBitsKHR transform, const std::vector<std::string> &lines);

constexpr uint32_t kMaxLines = 3;
constexpr uint32_t kMaxColumns = 32;

// Bit layout of the packed push constant fields. The overlay shaders unpack the same layout.
constexpr uint32_t kExtentHeightShift = 16;
constexpr uint32_t kExtentWidthMask = 0xFFFF;
constexpr uint32_t kParamsPixelSizeMask = 0xFF;
constexpr uint32_t kParamsRotationShift = 8;
constexpr uint32_t kParamsRotationMask = 0x3;
constexpr uint32_t kParamsLineCountShift = 10;
constexpr uint32_t kParamsOpaqueShift = 12;
constexpr uint32_t kGlyphsPerWord = 4;
constexpr uint32_t kBitsPerGlyph = 8;

// maxPushConstantsSize every Vulkan implementation supports
constexpr size_t kMinMaxPushConstantsSize = 128;

// Everything the overlay shaders need. Must match the push constant block in the overlay shaders.
struct OverlayPushConstants {
    int32_t visible_rect[4];  // Panel as the user sees it
    uint32_t visible_extent;  // width | height << 16
    uint32_t params;          // pixel size (bits 0-7) | rotation (8-9) | line count (10-11) | opaque (12)
    float white;              // Value of white for the swapchain color space
    uint32_t reserved;
    uint32_t glyphs[kMaxLines * kMaxColumns / kGlyphsPerWord];  // Glyph indices, one byte each, kMaxColumns per line
};
static_assert(sizeof(OverlayPushConstants) == kMinMaxPushConstantsSize,
              "Push constants must fit in the minimum maxPushConstantsSize");

OverlayPushConstants PackOverlayPushConstants(const OverlayLayout &layout, const std::vector<std::string> &lines, float white,
                                              bool opaque);

struct WhiteLevel {
    float value = 1.0f;
    bool known = true;  // False if the color space is not recognized and 1.0 was used
};

// Value written for white text, chosen so HDR swapchains show it at about 200 nits.
WhiteLevel WhiteForColorSpace(VkColorSpaceKHR color_space);

// Returns a copy of the create info that lets the overlay render into the swapchain images. The pNext chain is shared.
VkSwapchainCreateInfoKHR PatchSwapchainCreateInfo(const VkSwapchainCreateInfoKHR &create_info);

struct Eligibility {
    bool ok = true;
    const char *reason = nullptr;  // Why the overlay can't be drawn, when !ok
    bool deferred = false;         // Image memory is bound on first acquire, so image views must wait until then
};

Eligibility SwapchainOverlayEligibility(const VkSwapchainCreateInfoKHR &create_info);

bool ShouldDrawOverlay(VkQueueFlags queue_flags, bool any_overlay_swapchain, bool any_line_visible);

constexpr int kDefaultScalePercent = 100;
constexpr int kMinScalePercent = 50;
constexpr int kMaxScalePercent = 400;
constexpr int kDefaultUpdateIntervalMs = 500;
constexpr int kMinUpdateIntervalMs = 50;

// Layer settings as read, before validation
struct RawSettings {
    std::string position = "TOP_LEFT";
    int32_t scale = kDefaultScalePercent;
    int32_t update_interval_ms = kDefaultUpdateIntervalMs;
    bool show_fps = true;
    bool show_frame_time = true;
    bool show_frame_number = true;
};

struct MonitorSettings {
    Position position = Position::TopLeft;
    int scale_percent = kDefaultScalePercent;
    std::chrono::milliseconds update_interval{kDefaultUpdateIntervalMs};
    LineFlags lines;
};

MonitorSettings ParseSettings(const RawSettings &raw);

}  // namespace monitor
