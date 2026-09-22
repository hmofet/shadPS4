// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <string>
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

enum Hand : u32 {
    HandLeft = 0,
    HandRight = 1,
    HandCount = 2,
};

// The state of one tracked controller, in the terms of a DualShock 4 half: a stick, a trigger,
// a grip, two face buttons, the stick click and the menu button.
struct HandInput {
    bool active = false;    // the runtime has a controller bound for this hand
    float stick_x = 0.0f;   // -1 left .. 1 right
    float stick_y = 0.0f;   // -1 down .. 1 up
    float trigger = 0.0f;   // 0 .. 1
    float squeeze = 0.0f;   // 0 .. 1
    bool primary = false;   // A on the right, X on the left
    bool secondary = false; // B on the right, Y on the left
    bool stick_click = false;
    bool menu = false;
};

struct ControllerInput {
    std::array<HandInput, HandCount> hands{};
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

    // Friendly name of the headset's audio output device, as the host audio APIs list it, when
    // the runtime reports one (XR_OCULUS_audio_device_guid, Windows only). Empty otherwise.
    const std::string& GetAudioOutputDevice() const;

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

    // Pacing wait for the next frame, from any thread, ahead of BeginFrame. The frame state it
    // returns is queued for the next BeginFrame, so the wait can happen on the game's submit
    // thread while the GPU thread is still busy with the previous frame (xrWaitFrame only blocks
    // until the previous xrBeginFrame). False when the session is not running.
    bool WaitFrame();
    // Begins and ends (with no layers) a frame WaitFrame paced for a submit that will not render
    // after all, so the next xrWaitFrame is not left waiting for its xrBeginFrame. Same thread
    // rules as BeginFrame.
    void DiscardPendingFrame();
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
    // Reads the controllers' buttons and axes (xrSyncActions). False when the session is not
    // focused or no action set could be attached; the game keeps whatever input it had.
    bool SyncInput(ControllerInput& out);
    // Grip pose of one controller at a guest time, in tracking space. False when untracked.
    bool LocateHand(Hand hand, u64 guest_time_us, Pose& out);

    bool Sample(u64 guest_time_us, TrackingSample& out) override;
    void Recenter() override;
    FovTangents GetEyeFov(Eye eye) const override;
    float GetIpd() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VR
