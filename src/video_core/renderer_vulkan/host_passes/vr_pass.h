//  SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include "common/types.h"
#include "core/vr/vr_service.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan::HostPasses {

// Turns the guest's flipped frame into per-eye images for the VR headset.
//
// Each eye is blitted out of the frame (its half, or the whole frame in mono mode) into an
// intermediate image, then copied into that eye's OpenXR swapchain image. The two steps exist
// because the guest's output is already display encoded: the blit converts the pixel format
// without touching the encoding, and the copy moves those bits unchanged into the runtime's sRGB
// swapchain, which then decodes them correctly. A direct blit into the sRGB image would encode
// them a second time.
class VrPass {
public:
    struct Output {
        // Set when the desktop mirror should show one eye instead of the whole frame.
        vk::ImageView mirror_view{};
        vk::Extent2D mirror_extent{};
    };

    // Where one eye's picture is: a horizontal range of one array layer of a guest image.
    struct EyeSource {
        vk::Image image{};
        vk::Format format{};
        vk::Extent2D extent{}; // of the whole image
        u32 layer = 0;
        s32 x0 = 0;
        s32 x1 = 0;
        bool linear_filter = true;
    };

    void Create(vk::Device device, VmaAllocator allocator);

    // Source images must be in eTransferSrcOptimal. `display_view_format` is the format the
    // presenter samples the frame with, so a mirrored eye is sampled the same way.
    Output Render(vk::CommandBuffer cmdbuf, const std::array<EyeSource, VR::EyeCount>& src,
                  vk::Format display_view_format, const VR::FrameSubmit& frame);

private:
    struct EyeImage {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        vk::Extent2D extent{};
        vk::Format storage_format{};
        vk::Format view_format{};
    };

    void EnsureEyeImage(EyeImage& eye, vk::Extent2D extent, vk::Format storage_format,
                        vk::Format view_format);

    vk::Device device{};
    VmaAllocator allocator{};
    std::array<EyeImage, VR::EyeCount> eyes;
};

} // namespace Vulkan::HostPasses
