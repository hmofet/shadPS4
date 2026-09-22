//  SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/host_passes/vr_pass.h"
#include "video_core/renderer_vulkan/vk_platform.h"

namespace Vulkan::HostPasses {

namespace {

bool IsSrgb(vk::Format format) {
    switch (format) {
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eB8G8R8A8Srgb:
    case vk::Format::eA8B8G8R8SrgbPack32:
        return true;
    default:
        return false;
    }
}

// The eye images use the same channel order as the swapchain, in the encoding asked for.
vk::Format WithEncoding(vk::Format format, bool srgb) {
    switch (format) {
    case vk::Format::eB8G8R8A8Srgb:
    case vk::Format::eB8G8R8A8Unorm:
        return srgb ? vk::Format::eB8G8R8A8Srgb : vk::Format::eB8G8R8A8Unorm;
    default:
        return srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
    }
}

constexpr vk::ImageSubresourceRange ColorRange{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

constexpr vk::ImageSubresourceLayers ColorLayers{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .mipLevel = 0,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

vk::ImageMemoryBarrier2 Barrier(vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
                                vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    return {
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .image = image,
        .subresourceRange = ColorRange,
    };
}

void PipelineBarrier(vk::CommandBuffer cmdbuf, std::span<const vk::ImageMemoryBarrier2> barriers) {
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
        .pImageMemoryBarriers = barriers.data(),
    });
}

} // namespace

void VrPass::Create(vk::Device device_, VmaAllocator allocator_) {
    device = device_;
    allocator = allocator_;
}

void VrPass::EnsureEyeImage(EyeImage& eye, vk::Extent2D extent, vk::Format storage_format,
                            vk::Format view_format) {
    if (eye.image && eye.extent == extent && eye.storage_format == storage_format &&
        eye.view_format == view_format) {
        return;
    }
    if (eye.image) {
        // Rare (headset or frame size change); make sure the old image is no longer in use.
        Check(device.waitIdle());
        eye.view.reset();
        eye.image.Destroy();
    }

    const vk::ImageCreateInfo image_info{
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = storage_format,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc |
                 vk::ImageUsageFlagBits::eSampled,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    eye.image = VideoCore::UniqueImage(device, allocator);
    eye.image.Create(image_info);
    SetObjectName(device, static_cast<vk::Image>(eye.image), "VR eye image");

    eye.view = Check<"create VR eye image view">(device.createImageViewUnique({
        .image = eye.image,
        .viewType = vk::ImageViewType::e2D,
        .format = view_format,
        .subresourceRange = ColorRange,
    }));
    eye.extent = extent;
    eye.storage_format = storage_format;
    eye.view_format = view_format;
    LOG_INFO(Render_Vulkan, "VR eye images {}x{}, format {}", extent.width, extent.height,
             vk::to_string(storage_format));
}

VrPass::Output VrPass::Render(vk::CommandBuffer cmdbuf,
                              const std::array<EyeSource, VR::EyeCount>& src,
                              vk::Format display_view_format, const VR::FrameSubmit& frame) {
    const bool to_headset = frame.xr_render;
    const bool mirror_eye = frame.mirror != VR::MirrorMode::Full;
    if (!to_headset && !mirror_eye) {
        return {};
    }

    // Pick sizes and formats. With the headset displaying, eye images match its swapchain so
    // they can be copied straight in; otherwise any existing size will do for the mirror.
    for (u32 i = 0; i < VR::EyeCount; i++) {
        const bool src_srgb = IsSrgb(src[i].format);
        vk::Extent2D extent;
        vk::Format base;
        if (to_headset) {
            extent = {frame.targets[i].width, frame.targets[i].height};
            base = static_cast<vk::Format>(frame.targets[i].format);
        } else if (eyes[i].image) {
            extent = eyes[i].extent;
            base = eyes[i].storage_format;
        } else {
            extent = {static_cast<u32>(src[i].x1 - src[i].x0), src[i].extent.height};
            base = vk::Format::eR8G8B8A8Unorm;
        }
        EnsureEyeImage(eyes[i], extent, WithEncoding(base, src_srgb),
                       WithEncoding(base, IsSrgb(display_view_format)));
    }

    // Eye images: discard old contents, prepare for the blit.
    {
        std::array<vk::ImageMemoryBarrier2, VR::EyeCount> barriers;
        for (u32 i = 0; i < VR::EyeCount; i++) {
            barriers[i] = Barrier(
                eyes[i].image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eNone,
                vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferWrite);
        }
        PipelineBarrier(cmdbuf, barriers);
    }

    for (u32 i = 0; i < VR::EyeCount; i++) {
        const vk::ImageSubresourceLayers src_layers{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .mipLevel = 0,
            .baseArrayLayer = src[i].layer,
            .layerCount = 1,
        };
        const vk::ImageBlit region{
            .srcSubresource = src_layers,
            .srcOffsets =
                std::array{vk::Offset3D{src[i].x0, 0, 0},
                           vk::Offset3D{src[i].x1, static_cast<s32>(src[i].extent.height), 1}},
            .dstSubresource = ColorLayers,
            .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                                     vk::Offset3D{static_cast<s32>(eyes[i].extent.width),
                                                  static_cast<s32>(eyes[i].extent.height), 1}},
        };
        cmdbuf.blitImage(src[i].image, vk::ImageLayout::eTransferSrcOptimal, eyes[i].image,
                         vk::ImageLayout::eTransferDstOptimal, region,
                         src[i].linear_filter ? vk::Filter::eLinear : vk::Filter::eNearest);
    }

    if (to_headset) {
        // Eye images become copy sources; swapchain images move out of the layout the runtime
        // hands them over in.
        std::array<vk::ImageMemoryBarrier2, VR::EyeCount * 2> barriers;
        for (u32 i = 0; i < VR::EyeCount; i++) {
            barriers[i * 2] =
                Barrier(eyes[i].image, vk::ImageLayout::eTransferDstOptimal,
                        vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eBlit,
                        vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eCopy,
                        vk::AccessFlagBits2::eTransferRead);
            barriers[i * 2 + 1] =
                Barrier(vk::Image{frame.targets[i].image}, vk::ImageLayout::eColorAttachmentOptimal,
                        vk::ImageLayout::eTransferDstOptimal,
                        vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eNone,
                        vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
        }
        PipelineBarrier(cmdbuf, barriers);

        for (u32 i = 0; i < VR::EyeCount; i++) {
            const vk::ImageCopy copy{
                .srcSubresource = ColorLayers,
                .dstSubresource = ColorLayers,
                .extent = {eyes[i].extent.width, eyes[i].extent.height, 1},
            };
            cmdbuf.copyImage(eyes[i].image, vk::ImageLayout::eTransferSrcOptimal,
                             vk::Image{frame.targets[i].image},
                             vk::ImageLayout::eTransferDstOptimal, copy);
        }

        // Hand the swapchain images back in the layout the runtime expects; eye images go to
        // shader read for the mirror.
        for (u32 i = 0; i < VR::EyeCount; i++) {
            barriers[i * 2] = Barrier(
                eyes[i].image, vk::ImageLayout::eTransferSrcOptimal,
                vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferRead, vk::PipelineStageFlagBits2::eFragmentShader,
                vk::AccessFlagBits2::eShaderRead);
            barriers[i * 2 + 1] =
                Barrier(vk::Image{frame.targets[i].image}, vk::ImageLayout::eTransferDstOptimal,
                        vk::ImageLayout::eColorAttachmentOptimal, vk::PipelineStageFlagBits2::eCopy,
                        vk::AccessFlagBits2::eTransferWrite,
                        vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryRead);
        }
        PipelineBarrier(cmdbuf, barriers);
    } else {
        std::array<vk::ImageMemoryBarrier2, VR::EyeCount> barriers;
        for (u32 i = 0; i < VR::EyeCount; i++) {
            barriers[i] = Barrier(
                eyes[i].image, vk::ImageLayout::eTransferDstOptimal,
                vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eBlit,
                vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eFragmentShader,
                vk::AccessFlagBits2::eShaderRead);
        }
        PipelineBarrier(cmdbuf, barriers);
    }

    Output out{};
    if (mirror_eye) {
        const auto& eye = eyes[frame.mirror == VR::MirrorMode::Left ? VR::EyeLeft : VR::EyeRight];
        out.mirror_view = *eye.view;
        out.mirror_extent = eye.extent;
    }
    return out;
}

} // namespace Vulkan::HostPasses
