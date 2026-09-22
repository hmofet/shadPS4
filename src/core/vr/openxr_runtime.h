// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
// Same Vulkan configuration as video_core/renderer_vulkan/vk_common.h, so the result does not
// depend on which of the two headers a file includes first.
#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include "core/vr/pose_source.h"

namespace VR {

// One eye's swapchain image for the current frame, ready to be written with transfer commands.
// The image is in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL and must be left in that layout.
struct EyeTarget {
    VkImage image = VK_NULL_HANDLE;
    u32 width = 0;
    u32 height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
};

// What the renderer used for one eye: submitted back to the runtime with the image so it can
// reproject from the right place.
struct EyeView {
    Pose pose{};
    FovTangents fov{};
};

// An OpenXR instance, system and session sharing the emulator's Vulkan device
// (XR_KHR_vulkan_enable2). Doubles as the pose source.
class OpenXrRuntime final : public PoseSource {
public:
    OpenXrRuntime();
    ~OpenXrRuntime() override;

    // Creates the instance and finds a head-mounted system. False means no usable runtime or
    // headset, and the caller should fall back to another pose source.
    bool Initialize();

    // Vulkan interop. The emulator creates its instance and device through these so the runtime
    // can add what it needs, and uses the physical device the runtime is attached to.
    VkResult CreateVulkanInstance(const VkInstanceCreateInfo* create_info,
                                  PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                                  VkInstance* out_instance);
    VkPhysicalDevice GetVulkanPhysicalDevice(VkInstance instance);
    VkResult CreateVulkanDevice(VkPhysicalDevice physical_device,
                                const VkDeviceCreateInfo* create_info,
                                PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                                VkDevice* out_device);
    bool CreateSession(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
                       u32 queue_family_index, u32 queue_index);
    // Must run before the Vulkan device is destroyed.
    void DestroySession();

    // Handles runtime events (session start/stop). Cheap; call often.
    void PollEvents();
    bool IsSessionRunning() const;

    // Frame loop. BeginFrame blocks in xrWaitFrame for pacing, then acquires the eye images.
    // Returns false if no frame was begun. When it returns true, EndFrame must follow; the
    // `targets` are valid only if `should_render` is true.
    bool BeginFrame(std::array<EyeTarget, EyeCount>& targets, bool& should_render);
    // Releases the eye images and submits them as a projection layer (or an empty frame when
    // `rendered` is false). The caller must hold the emulator's queue submit lock, because the
    // runtime may submit to the shared queue.
    void EndFrame(bool rendered, const std::array<EyeView, EyeCount>& views);

    // PoseSource
    std::string_view Name() const override {
        return "openxr";
    }
    bool Sample(u64 guest_time_us, TrackingSample& out) override;
    void Recenter() override;
    FovTangents GetEyeFov(Eye eye) const override;
    float GetIpd() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VR
