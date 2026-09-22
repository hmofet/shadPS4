// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <SDL3/SDL_keyboard.h>

#include "common/singleton.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/pad/pad.h"
#include "core/vr/vr_service.h"
#include "input/controller.h"

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
    u32 hmd_refresh_hz = 0;
    bool controllers_enabled = true;

    std::atomic<bool> reprojection_active{false};

    // Controller input as last applied to the pad, so only changes are pushed.
    std::mutex input_mutex;
    u64 last_input_sync_us = 0;
    bool input_connected = false;
#ifdef ENABLE_OPENXR
    ControllerInput last_input{};
#endif

    // The pose and FOV the game was last given, reported back with each submitted frame that
    // does not say which pose it used.
    TrackingSample render_sample{};
    HmdFov reported_fov{};
    bool fov_reported = false;
    // Recent head samples given to the game, so a submitted frame's pose can be matched to the
    // exact eye poses it rendered with. A ring; the game is at most a few frames behind.
    static constexpr size_t RecentSamples = 32;
    std::array<TrackingSample, RecentSamples> recent{};
    size_t recent_next = 0;

    // Game frame rate for the log.
    u64 rate_window_start_us = 0;
    u64 rate_window_frames = 0;
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
    s.controllers_enabled = EmulatorSettings.IsVrControllersEnabled();
    s.hmd_refresh_hz = EmulatorSettings.GetVrHmdRefreshHz();
    if (s.hmd_refresh_hz != 0 && (s.hmd_refresh_hz < 30 || s.hmd_refresh_hz > 360)) {
        LOG_WARNING(Lib_Hmd, "VR hmd_refresh_hz {} is out of range, using 120", s.hmd_refresh_hz);
        s.hmd_refresh_hz = 120;
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
             "PSVR enabled: pose source '{}', eyes '{}', mirror '{}', fov '{}', render scale {}, "
             "VR mode vblank {} Hz",
             s.source.load()->Name(), s.eye_source == EyeSource::Mono ? "mono" : "sbs", mirror,
             s.native_fov ? "native" : "psvr", s.render_scale, s.hmd_refresh_hz);
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

std::string GetHeadsetAudioDevice() {
    if (!IsPsvrEnabled()) {
        return {};
    }
#ifdef ENABLE_OPENXR
    if (OpenXrRuntime* xr = GetOpenXr(Get())) {
        return xr->GetAudioOutputDevice();
    }
#endif
    return {};
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
    s.recent[s.recent_next] = out;
    s.recent_next = (s.recent_next + 1) % State::RecentSamples;
    return true;
}

namespace {

// The FOV reported to the game, as the tangents of each eye's frustum.
std::array<FovTangents, EyeCount> ReportedEyeFov(State& s) {
    HmdFov fov;
    bool reported;
    {
        std::scoped_lock lk{s.mutex};
        fov = s.reported_fov;
        reported = s.fov_reported;
    }
    if (!reported) {
        fov = GetReportedFov();
    }
    return {FovTangents{fov.tan_out, fov.tan_in, fov.tan_top, fov.tan_bottom},
            FovTangents{fov.tan_in, fov.tan_out, fov.tan_top, fov.tan_bottom}};
}

} // namespace

HmdFrameViews ResolveRenderViews(const HmdFrameInfo& info) {
    State& s = Get();
    TrackingSample sample;
    bool matched = false;
    {
        std::scoped_lock lk{s.mutex};
        sample = s.render_sample;
        if (info.have_pose) {
            for (const TrackingSample& recent : s.recent) {
                if (recent.valid && SamePose(recent.head, info.head)) {
                    sample = recent;
                    matched = true;
                    break;
                }
            }
        }
    }
    if (info.have_pose && !matched) {
        // The game rendered from a pose it made itself (smoothed, or clamped for comfort). Use
        // it as given, with the eyes on either side of it.
        const float half_ipd = GetIpd() * 0.5f;
        sample = {};
        sample.valid = true;
        sample.head = info.head;
        sample.guest_time_us = info.guest_time_us;
        sample.eyes[EyeLeft] = Compose(info.head, {{}, {-half_ipd, 0.0f, 0.0f}});
        sample.eyes[EyeRight] = Compose(info.head, {{}, {half_ipd, 0.0f, 0.0f}});
    }
    const std::array<FovTangents, EyeCount> reported_fov = ReportedEyeFov(s);

    HmdFrameViews views{};
    for (u32 eye = 0; eye < EyeCount; eye++) {
        views[eye].pose = sample.eyes[eye];
        views[eye].fov = info.have_fov ? info.fov[eye] : reported_fov[eye];
    }
    VR_TRACE(Lib_Hmd,
             "render views: pose {} ({}), fov {} (left eye l={:.3f} r={:.3f} u={:.3f} d={:.3f})",
             info.have_pose ? "from the game" : "last sample",
             matched ? "matched a sample" : "unmatched",
             info.have_fov ? "from the game" : "as reported", views[EyeLeft].fov.left,
             views[EyeLeft].fov.right, views[EyeLeft].fov.up, views[EyeLeft].fov.down);
    return views;
}

void Recenter() {
    Get().source.load()->Recenter();
}

bool SampleController(bool right_hand, u64 guest_time_us, Pose& out) {
#ifdef ENABLE_OPENXR
    State& s = Get();
    if (!s.controllers_enabled) {
        return false;
    }
    if (OpenXrRuntime* xr = GetOpenXr(s)) {
        return xr->LocateHand(right_hand ? HandRight : HandLeft, guest_time_us, out);
    }
#endif
    return false;
}

#ifdef ENABLE_OPENXR
namespace {

using Libraries::Pad::OrbisPadButtonDataOffset;

int StickValue(float v) {
    // OpenXR sticks are -1..1 with +y up; the pad wants 0..255 with y down.
    const int units = static_cast<int>(std::lround(std::clamp(v, -1.0f, 1.0f) * 127.0f));
    return Input::GetAxis(-0x80, 0x7f, units);
}

int TriggerValue(float v) {
    return Input::GetAxis(0, 0x7f,
                          static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 127.0f)));
}

// Pushes what changed since the last poll to the first pad. Called with input_mutex held.
void ApplyControllerInput(State& s, const ControllerInput& in) {
    auto& controllers = *Common::Singleton<Input::GameControllers>::Instance();
    Input::GameController* pad = controllers[0];
    const ControllerInput& old = s.last_input;

    const bool active = in.hands[HandLeft].active || in.hands[HandRight].active;
    if (active && !s.input_connected) {
        // Show up as a connected pad when no physical one is; a physical pad keeps its handle.
        if (pad->m_sdl_gamepad == nullptr) {
            pad->ConnectController(nullptr);
        }
        s.input_connected = true;
        LOG_INFO(Lib_Hmd, "OpenXR: controllers active, mapped onto the DualShock 4");
    }

    const auto button = [&](bool now, bool before, OrbisPadButtonDataOffset offset) {
        if (now != before) {
            pad->Button(offset, now);
        }
    };
    const auto axis = [&](float now, float before, Input::Axis which, bool stick, bool invert) {
        const int a = stick ? StickValue(invert ? -now : now) : TriggerValue(now);
        const int b = stick ? StickValue(invert ? -before : before) : TriggerValue(before);
        if (a != b) {
            pad->Axis(which, a, false);
        }
    };

    const HandInput& l = in.hands[HandLeft];
    const HandInput& r = in.hands[HandRight];
    const HandInput& ol = old.hands[HandLeft];
    const HandInput& orr = old.hands[HandRight];

    axis(l.stick_x, ol.stick_x, Input::Axis::LeftX, true, false);
    axis(l.stick_y, ol.stick_y, Input::Axis::LeftY, true, true);
    axis(r.stick_x, orr.stick_x, Input::Axis::RightX, true, false);
    axis(r.stick_y, orr.stick_y, Input::Axis::RightY, true, true);
    axis(l.trigger, ol.trigger, Input::Axis::TriggerLeft, false, false);
    axis(r.trigger, orr.trigger, Input::Axis::TriggerRight, false, false);

    constexpr float SqueezeOn = 0.5f;
    button(l.squeeze > SqueezeOn, ol.squeeze > SqueezeOn, OrbisPadButtonDataOffset::L1);
    button(r.squeeze > SqueezeOn, orr.squeeze > SqueezeOn, OrbisPadButtonDataOffset::R1);
    button(r.primary, orr.primary, OrbisPadButtonDataOffset::Cross);
    button(r.secondary, orr.secondary, OrbisPadButtonDataOffset::Circle);
    button(l.primary, ol.primary, OrbisPadButtonDataOffset::Square);
    button(l.secondary, ol.secondary, OrbisPadButtonDataOffset::Triangle);
    button(l.stick_click, ol.stick_click, OrbisPadButtonDataOffset::L3);
    button(r.stick_click, orr.stick_click, OrbisPadButtonDataOffset::R3);
    button(l.menu || r.menu, ol.menu || orr.menu, OrbisPadButtonDataOffset::Options);

    s.last_input = in;
}

} // namespace
#endif

void PollInput() {
#ifdef ENABLE_OPENXR
    State& s = Get();
    if (!s.controllers_enabled) {
        return;
    }
    OpenXrRuntime* xr = GetOpenXr(s);
    if (xr == nullptr) {
        return;
    }
    constexpr u64 SyncIntervalUs = 4'000;
    const u64 now = Libraries::Kernel::sceKernelGetProcessTime();
    std::scoped_lock lk{s.input_mutex};
    if (now - s.last_input_sync_us < SyncIntervalUs) {
        return;
    }
    s.last_input_sync_us = now;
    ControllerInput in;
    if (xr->SyncInput(in)) {
        ApplyControllerInput(s, in);
    }
#endif
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

u32 GetHmdVblankHz() {
    if (!IsPsvrEnabled()) {
        return 0;
    }
    return Get().hmd_refresh_hz;
}

void OnHmdFrameAccepted() {
    State& s = Get();
    {
        // Frame rate to the log every few seconds: the first thing to check when VR feels wrong.
        constexpr u64 WindowUs = 5'000'000;
        const u64 now = Libraries::Kernel::sceKernelGetProcessTime();
        std::scoped_lock lk{s.mutex};
        s.rate_window_frames++;
        if (s.rate_window_start_us == 0) {
            s.rate_window_start_us = now;
        } else if (now - s.rate_window_start_us >= WindowUs) {
            const double seconds = static_cast<double>(now - s.rate_window_start_us) * 1e-6;
            LOG_INFO(Lib_Hmd, "VR: game submits {:.1f} frames/s", s.rate_window_frames / seconds);
            s.rate_window_start_us = now;
            s.rate_window_frames = 0;
        }
    }
#ifdef ENABLE_OPENXR
    if (OpenXrRuntime* xr = GetOpenXr(s)) {
        xr->WaitFrame();
    }
#endif
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
    PollInput();
    frame.vr_active = s.reprojection_active.load();
    if (!frame.vr_active) {
#ifdef ENABLE_OPENXR
        // The game left VR mode after this frame was accepted (OnHmdFrameAccepted): its wait
        // must still be paired with a begin, or the next wait never returns.
        if (xr) {
            xr->DiscardPendingFrame();
        }
#endif
        return frame;
    }
    frame.eye_source = s.eye_source;
    frame.mirror = s.mirror;
#ifdef ENABLE_OPENXR
    if (xr) {
        // BeginFrame consumes the pending wait even when the session has stopped meanwhile.
        bool should_render = false;
        if (xr->BeginFrame(frame.targets, should_render)) {
            frame.xr_frame = true;
            frame.xr_render = should_render;
        }
    }
#endif
    return frame;
}

void EndFrame(const FrameSubmit& frame, bool rendered, const HmdFrameViews* views) {
    if (!frame.xr_frame) {
        return;
    }
#ifdef ENABLE_OPENXR
    State& s = Get();
    OpenXrRuntime* xr = GetOpenXr(s);
    if (!xr) {
        return;
    }
    if (views != nullptr) {
        xr->EndFrame(rendered && frame.xr_render, *views);
        return;
    }
    // No pose came with the frame (the flipped frame is being split): the last one given to the
    // game is the best guess.
    TrackingSample sample;
    {
        std::scoped_lock lk{s.mutex};
        sample = s.render_sample;
    }
    const std::array<FovTangents, EyeCount> fov = ReportedEyeFov(s);
    HmdFrameViews last{};
    for (u32 eye = 0; eye < EyeCount; eye++) {
        last[eye] = {sample.eyes[eye], fov[eye]};
    }
    xr->EndFrame(rendered && frame.xr_render && sample.valid, last);
#endif
}

} // namespace VR
