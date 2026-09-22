// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <SDL3/SDL_keyboard.h>

#include "core/emulator_settings.h"
#include "core/libraries/kernel/time.h"
#include "core/vr/vr_service.h"

namespace VR {

namespace {

struct State {
    std::once_flag init_once;
    std::mutex mutex;

    std::unique_ptr<PoseSource> fallback_source;
#ifdef ENABLE_OPENXR
    std::unique_ptr<OpenXrRuntime> openxr;
#endif
    // Points at openxr or fallback_source.
    std::atomic<PoseSource*> source{nullptr};

    EyeSource eye_source = EyeSource::SideBySide;
    MirrorMode mirror = MirrorMode::Full;
    bool native_fov = true;
    float render_scale = 1.0f;
    SDL_Scancode recenter_key = SDL_SCANCODE_UNKNOWN;
    bool recenter_key_down = false;

    std::atomic<bool> reprojection_active{false};

    // The pose and FOV the game was last given, reported back with each submitted frame.
    TrackingSample render_sample{};
    HmdFov reported_fov{};
    bool fov_reported = false;
};

State& GetState() {
    static State state;
    return state;
}

void Initialize(State& s) {
    s.eye_source =
        EmulatorSettings.GetVrEyeSource() == "mono" ? EyeSource::Mono : EyeSource::SideBySide;
    const std::string mirror = EmulatorSettings.GetVrMirror();
    s.mirror = mirror == "left"    ? MirrorMode::Left
               : mirror == "right" ? MirrorMode::Right
                                   : MirrorMode::Full;
    s.native_fov = EmulatorSettings.GetVrFovMode() != "psvr";
    s.render_scale = std::clamp(EmulatorSettings.GetVrRenderScale(), 0.5f, 4.0f);
    const std::string key = EmulatorSettings.GetVrRecenterKey();
    s.recenter_key = SDL_GetScancodeFromName(key.c_str());
    if (!key.empty() && s.recenter_key == SDL_SCANCODE_UNKNOWN) {
        LOG_WARNING(Lib_Hmd, "Unknown VR recenter key '{}'", key);
    }

    const std::string wanted = EmulatorSettings.GetVrPoseSource();
    s.fallback_source = std::make_unique<DeskPoseSource>(wanted != "static");
    s.source = s.fallback_source.get();

    if (wanted == "openxr") {
#ifdef ENABLE_OPENXR
        auto runtime = std::make_unique<OpenXrRuntime>();
        if (runtime->Initialize()) {
            s.openxr = std::move(runtime);
            s.source = s.openxr.get();
        } else {
            LOG_WARNING(Lib_Hmd, "No OpenXR headset available, using the keyboard pose source");
        }
#else
        LOG_WARNING(Lib_Hmd, "Built without OpenXR support, using the keyboard pose source");
#endif
    }

    LOG_INFO(Lib_Hmd,
             "PSVR enabled: pose source '{}', eyes '{}', mirror '{}', fov '{}', render scale {}",
             s.source.load()->Name(), s.eye_source == EyeSource::Mono ? "mono" : "sbs", mirror,
             s.native_fov ? "native" : "psvr", s.render_scale);
}

State& Get() {
    State& s = GetState();
    std::call_once(s.init_once, [&s] { Initialize(s); });
    return s;
}

#ifdef ENABLE_OPENXR
OpenXrRuntime* GetOpenXr(State& s) {
    return s.source.load() == s.openxr.get() ? s.openxr.get() : nullptr;
}
#endif

} // namespace

bool IsPsvrEnabled() {
    // Latched on first use: the Vulkan device and the libraries must agree for the whole run.
    static const bool enabled = EmulatorSettings.IsPsvrEnabled();
    return enabled;
}

std::string_view GetPoseSourceName() {
    return Get().source.load()->Name();
}

HmdFov GetReportedFov() {
    State& s = Get();
    HmdFov fov;
    if (s.native_fov) {
        // The PSVR API has one FOV for both eyes, mirrored. Average the headset's two eyes.
        PoseSource* source = s.source.load();
        // Games ask once at startup, often before any tracking query; a sample makes the runtime
        // report its views so the FOV below is the headset's own.
        TrackingSample unused;
        source->Sample(Libraries::Kernel::sceKernelGetProcessTime(), unused);
        const FovTangents l = source->GetEyeFov(EyeLeft);
        const FovTangents r = source->GetEyeFov(EyeRight);
        fov = {(l.left + r.right) * 0.5f, (l.right + r.left) * 0.5f, (l.up + r.up) * 0.5f,
               (l.down + r.down) * 0.5f};
    } else {
        fov = {PsvrFov.left, PsvrFov.right, PsvrFov.up, PsvrFov.down};
    }
    std::scoped_lock lk{s.mutex};
    s.reported_fov = fov;
    s.fov_reported = true;
    return fov;
}

float GetIpd() {
    return Get().source.load()->GetIpd();
}

void GetPanelResolution(u32& width, u32& height) {
    const float scale = Get().render_scale;
    width = static_cast<u32>(1920 * scale);
    height = static_cast<u32>(1080 * scale);
}

bool SampleHead(u64 guest_time_us, bool is_render_pose, TrackingSample& out) {
    State& s = Get();
    if (!s.source.load()->Sample(guest_time_us, out)) {
        return false;
    }
    if (!is_render_pose) {
        return true;
    }
    std::scoped_lock lk{s.mutex};
    s.render_sample = out;
    return true;
}

void Recenter() {
    Get().source.load()->Recenter();
}

void PollRecenterKey() {
    State& s = Get();
    if (s.recenter_key == SDL_SCANCODE_UNKNOWN) {
        return;
    }
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const bool down = keys != nullptr && keys[s.recenter_key];
    if (down && !s.recenter_key_down) {
        Recenter();
    }
    s.recenter_key_down = down;
}

void SetReprojectionActive(bool active, std::string_view variant) {
    State& s = Get();
    if (s.reprojection_active.exchange(active) != active) {
        LOG_INFO(Lib_Hmd, "VR mode {} ({})", active ? "entered" : "left", variant);
    }
}

bool IsReprojectionActive() {
    return Get().reprojection_active.load();
}

bool UseOpenXrVulkan() {
    if (!IsPsvrEnabled()) {
        return false;
    }
#ifdef ENABLE_OPENXR
    return GetOpenXr(Get()) != nullptr;
#else
    return false;
#endif
}

void DisableOpenXr(std::string_view reason) {
    State& s = Get();
    LOG_ERROR(Lib_Hmd, "OpenXR disabled: {}. Using the keyboard pose source", reason);
#ifdef ENABLE_OPENXR
    if (s.openxr) {
        s.openxr->DestroySession();
    }
#endif
    s.source = s.fallback_source.get();
}

VkResult CreateVulkanInstance(const VkInstanceCreateInfo* create_info,
                              PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                              VkInstance* out_instance) {
#ifdef ENABLE_OPENXR
    if (auto* xr = GetOpenXr(Get())) {
        return xr->CreateVulkanInstance(create_info, get_instance_proc_addr, out_instance);
    }
#endif
    return VK_ERROR_INITIALIZATION_FAILED;
}

VkPhysicalDevice GetVulkanPhysicalDevice(VkInstance instance) {
#ifdef ENABLE_OPENXR
    if (auto* xr = GetOpenXr(Get())) {
        return xr->GetVulkanPhysicalDevice(instance);
    }
#endif
    return VK_NULL_HANDLE;
}

VkResult CreateVulkanDevice(VkPhysicalDevice physical_device, const VkDeviceCreateInfo* create_info,
                            PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                            VkDevice* out_device) {
#ifdef ENABLE_OPENXR
    if (auto* xr = GetOpenXr(Get())) {
        return xr->CreateVulkanDevice(physical_device, create_info, get_instance_proc_addr,
                                      out_device);
    }
#endif
    return VK_ERROR_INITIALIZATION_FAILED;
}

void OnVulkanDeviceCreated(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
                           u32 queue_family_index, u32 queue_index) {
#ifdef ENABLE_OPENXR
    if (auto* xr = GetOpenXr(Get())) {
        if (!xr->CreateSession(instance, physical_device, device, queue_family_index,
                               queue_index)) {
            DisableOpenXr("session creation failed");
            return;
        }
        xr->PollEvents();
    }
#endif
}

void OnVulkanDeviceDestroying() {
#ifdef ENABLE_OPENXR
    if (!IsPsvrEnabled()) {
        return;
    }
    if (auto* xr = GetOpenXr(Get())) {
        xr->DestroySession();
    }
#endif
}

FrameSubmit BeginFrame() {
    FrameSubmit frame;
    if (!IsPsvrEnabled()) {
        return frame;
    }
    State& s = Get();
#ifdef ENABLE_OPENXR
    OpenXrRuntime* xr = GetOpenXr(s);
    if (xr) {
        xr->PollEvents();
    }
#endif
    frame.vr_active = s.reprojection_active.load();
    if (!frame.vr_active) {
        return frame;
    }
    frame.eye_source = s.eye_source;
    frame.mirror = s.mirror;
#ifdef ENABLE_OPENXR
    if (xr && xr->IsSessionRunning()) {
        bool should_render = false;
        if (xr->BeginFrame(frame.targets, should_render)) {
            frame.xr_frame = true;
            frame.xr_render = should_render;
        }
    }
#endif
    return frame;
}

void EndFrame(const FrameSubmit& frame, bool rendered) {
    if (!frame.xr_frame) {
        return;
    }
#ifdef ENABLE_OPENXR
    State& s = Get();
    OpenXrRuntime* xr = GetOpenXr(s);
    if (!xr) {
        return;
    }
    TrackingSample sample;
    HmdFov fov;
    {
        std::scoped_lock lk{s.mutex};
        sample = s.render_sample;
        fov = s.fov_reported ? s.reported_fov : HmdFov{};
    }
    if (!s.fov_reported) {
        fov = GetReportedFov();
    }
    std::array<EyeView, EyeCount> views{};
    views[EyeLeft] = {sample.eyes[EyeLeft], {fov.tan_out, fov.tan_in, fov.tan_top, fov.tan_bottom}};
    views[EyeRight] = {sample.eyes[EyeRight],
                       {fov.tan_in, fov.tan_out, fov.tan_top, fov.tan_bottom}};
    xr->EndFrame(rendered && frame.xr_render && sample.valid, views);
#endif
}

} // namespace VR
