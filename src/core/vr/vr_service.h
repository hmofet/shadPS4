// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <string_view>
#include "common/logging/log.h"
#include "core/vr/openxr_runtime.h"
#include "core/vr/pose_source.h"

// Decoded call tracing for the VR libraries. Logs the first calls from each call site and then
// every 600th, at Debug level so it survives release builds; enable it with a log filter such as
// "Lib.Hmd:debug Lib.VrTracker:debug Lib.Camera:debug Lib.HmdSetupDialog:debug".
#define VR_TRACE(log_class, fmt, ...)                                                              \
    do {                                                                                           \
        static std::atomic<u64> vr_trace_count_{0};                                                \
        const u64 vr_trace_n_ = vr_trace_count_.fetch_add(1, std::memory_order_relaxed);           \
        if (vr_trace_n_ < 16 || vr_trace_n_ % 600 == 0) {                                          \
            LOG_DEBUG(log_class, "[#{}] " fmt, vr_trace_n_ __VA_OPT__(, ) __VA_ARGS__);            \
        }                                                                                          \
    } while (false)

namespace VR {

enum class EyeSource {
    SideBySide, // left half of the flipped frame to the left eye, right half to the right
    Mono,       // the whole frame to both eyes
};

enum class MirrorMode {
    Full,  // the desktop window shows the flipped frame as usual
    Left,  // only the left eye
    Right, // only the right eye
};

// The FOV as the PSVR API reports it: one symmetric pair of eyes, "out" being the temple side.
struct HmdFov {
    float tan_out = 0.0f;
    float tan_in = 0.0f;
    float tan_top = 0.0f;
    float tan_bottom = 0.0f;
};

// Distance from the PSVR camera to where the player's head starts. PSVR titles expect the camera
// in front of the player; OpenXR's LOCAL origin is the head itself, so the tracker library moves
// every pose back by this much along +Z.
constexpr float CameraDistance = 1.5f;

// True when the settings ask for a PSVR to be reported. Everything else here is a no-op when false.
bool IsPsvrEnabled();

// ---- Queries for the Hmd and VrTracker libraries -------------------------------------------

std::string_view GetPoseSourceName();
HmdFov GetReportedFov();
float GetIpd();
void GetPanelResolution(u32& width, u32& height);

// Samples the head at `guest_time_us` (guest process time). With `is_render_pose`, the sample is
// kept as the pose the game is rendering with, which the frame submit reports to the runtime.
bool SampleHead(u64 guest_time_us, bool is_render_pose, TrackingSample& out);
void Recenter();
// Recenters when the configured recenter key goes down. Called from the tracker's result path.
void PollRecenterKey();

// ---- Reprojection state ---------------------------------------------------------------------

void SetReprojectionActive(bool active, std::string_view variant);
bool IsReprojectionActive();

// ---- Vulkan interop, called by the Vulkan backend --------------------------------------------

// True when the Vulkan instance and device should be created through the OpenXR runtime.
bool UseOpenXrVulkan();
// Called when interop failed; the session is dropped and the keyboard pose source takes over.
void DisableOpenXr(std::string_view reason);
VkResult CreateVulkanInstance(const VkInstanceCreateInfo* create_info,
                              PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                              VkInstance* out_instance);
VkPhysicalDevice GetVulkanPhysicalDevice(VkInstance instance);
VkResult CreateVulkanDevice(VkPhysicalDevice physical_device, const VkDeviceCreateInfo* create_info,
                            PFN_vkGetInstanceProcAddr get_instance_proc_addr, VkDevice* out_device);
void OnVulkanDeviceCreated(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
                           u32 queue_family_index, u32 queue_index);
void OnVulkanDeviceDestroying();

// ---- Frame submission, called by the presenter on every flip --------------------------------

struct FrameSubmit {
    bool vr_active = false; // the game is in VR mode; eye images should be produced
    bool xr_frame = false;  // an OpenXR frame was begun; EndFrame must be called
    bool xr_render = false; // `targets` are valid and should be written
    EyeSource eye_source = EyeSource::SideBySide;
    MirrorMode mirror = MirrorMode::Full;
    std::array<EyeTarget, EyeCount> targets{};
};

// May block in xrWaitFrame for pacing when the headset is displaying.
FrameSubmit BeginFrame();
// The caller must hold the Vulkan queue submit lock; see OpenXrRuntime::EndFrame.
void EndFrame(const FrameSubmit& frame, bool rendered);

} // namespace VR
