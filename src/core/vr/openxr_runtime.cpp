// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// openxr_platform.h declares Win32 entry points that take IUnknown, which the lean header omits.
#include <unknwn.h>
#include <combaseapi.h>
#include <mmdeviceapi.h>
// Defines PKEY_Device_FriendlyName here rather than needing a library that exports it.
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>
#define XR_USE_PLATFORM_WIN32
#else
#include <time.h>
#define XR_USE_TIMESPEC
#endif
// Same Vulkan configuration as video_core/renderer_vulkan/vk_common.h, so the result does not
// depend on which of the two headers a file includes first.
#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "common/logging/log.h"
#include "core/libraries/kernel/time.h"
#include "core/vr/openxr_runtime.h"

namespace VR {

namespace {

constexpr XrViewConfigurationType ViewConfig = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

// Swapchain formats in order of preference. The guest's output is already display encoded, so an
// sRGB swapchain is used and the encoded values are copied in bit for bit (see vr_pass.cpp).
constexpr std::array<VkFormat, 4> PreferredFormats{
    VK_FORMAT_R8G8B8A8_SRGB,
    VK_FORMAT_B8G8R8A8_SRGB,
    VK_FORMAT_R8G8B8A8_UNORM,
    VK_FORMAT_B8G8R8A8_UNORM,
};

#ifdef _WIN32
// Friendly name of the audio endpoint with the given Windows device ID (the string
// XR_OCULUS_audio_device_guid returns), which is the name SDL and OpenAL list it under.
std::string AudioEndpointName(const wchar_t* device_id) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::string name;
    IMMDeviceEnumerator* enumerator = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&enumerator)))) {
        IMMDevice* device = nullptr;
        if (SUCCEEDED(enumerator->GetDevice(device_id, &device))) {
            IPropertyStore* props = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) &&
                    value.vt == VT_LPWSTR) {
                    const int size = WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, nullptr,
                                                         0, nullptr, nullptr);
                    if (size > 1) {
                        name.resize(size - 1);
                        WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, name.data(), size,
                                            nullptr, nullptr);
                    }
                }
                PropVariantClear(&value);
                props->Release();
            }
            device->Release();
        }
        enumerator->Release();
    }
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    return name;
}
#endif

XrPosef ToXr(const Pose& p) {
    return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
            {p.position.x, p.position.y, p.position.z}};
}

Pose FromXr(const XrPosef& p) {
    return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
            {p.position.x, p.position.y, p.position.z}};
}

XrFovf ToXr(const FovTangents& f) {
    return {-std::atan(f.left), std::atan(f.right), std::atan(f.up), -std::atan(f.down)};
}

FovTangents FromXr(const XrFovf& f) {
    return {std::tan(-f.angleLeft), std::tan(f.angleRight), std::tan(f.angleUp),
            std::tan(-f.angleDown)};
}

const char* SessionStateName(XrSessionState state) {
    switch (state) {
    case XR_SESSION_STATE_IDLE:
        return "IDLE";
    case XR_SESSION_STATE_READY:
        return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED:
        return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE:
        return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED:
        return "FOCUSED";
    case XR_SESSION_STATE_STOPPING:
        return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING:
        return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING:
        return "EXITING";
    default:
        return "UNKNOWN";
    }
}

} // namespace

struct OpenXrRuntime::Impl {
    struct Swapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        u32 width = 0;
        u32 height = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        std::vector<XrSwapchainImageVulkan2KHR> images;
        bool acquired = false;
        u32 acquired_index = 0;
    };

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local_space = XR_NULL_HANDLE;
    XrSpace view_space = XR_NULL_HANDLE;
    XrSpace app_space = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    bool instance_lost = false;

    std::array<XrViewConfigurationView, EyeCount> config_views{};
    std::array<Swapchain, EyeCount> swapchains{};
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    u64 guest_time_at_wait = 0;

    // Frames waited for (WaitFrame) but not yet begun (BeginFrame), oldest first.
    struct PendingFrame {
        XrFrameState state;
        u64 guest_time_at_wait;
    };
    std::deque<PendingFrame> pending_frames;
    static constexpr size_t MaxPendingFrames = 3;

    // Last good view data, for FOV and IPD queries.
    std::array<FovTangents, EyeCount> eye_fov{};
    bool have_fov = false;
    float ipd = PsvrIpd;

    // Controller input: one action set, actions with a subaction path per hand.
    XrActionSet action_set = XR_NULL_HANDLE;
    XrAction action_stick = XR_NULL_HANDLE;
    XrAction action_trigger = XR_NULL_HANDLE;
    XrAction action_squeeze = XR_NULL_HANDLE;
    XrAction action_primary = XR_NULL_HANDLE;
    XrAction action_secondary = XR_NULL_HANDLE;
    XrAction action_stick_click = XR_NULL_HANDLE;
    XrAction action_menu = XR_NULL_HANDLE;
    XrAction action_grip = XR_NULL_HANDLE;
    std::array<XrPath, HandCount> hand_paths{};
    std::array<XrSpace, HandCount> grip_spaces{};
    bool actions_attached = false;

    PFN_xrGetVulkanGraphicsRequirements2KHR get_graphics_requirements = nullptr;
    PFN_xrCreateVulkanInstanceKHR create_vulkan_instance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR get_vulkan_device = nullptr;
    PFN_xrCreateVulkanDeviceKHR create_vulkan_device = nullptr;
#ifdef _WIN32
    PFN_xrConvertWin32PerformanceCounterToTimeKHR convert_time = nullptr;
#else
    PFN_xrConvertTimespecTimeToTimeKHR convert_time = nullptr;
#endif
    std::string audio_output_device;
    bool has_refresh_rate_ext = false;

    // Guards everything above against the game threads that sample poses while the presenter
    // thread runs the frame loop. Never held across xrWaitFrame.
    mutable std::mutex mutex;

    // Asks for the highest headset refresh rate that is a multiple of 60 Hz. PSVR games run at
    // 60 frames per second (reprojected to 120 Hz on a PSVR), and 60 fps on a 90 Hz display
    // shows each frame for one, then two refreshes: judder, and xrWaitFrame pacing drops the
    // game to 45 fps once a frame takes longer than two 90 Hz slots.
    void RequestRefreshRate() {
        if (!has_refresh_rate_ext) {
            return;
        }
        PFN_xrEnumerateDisplayRefreshRatesFB enumerate = nullptr;
        PFN_xrGetDisplayRefreshRateFB get = nullptr;
        PFN_xrRequestDisplayRefreshRateFB request = nullptr;
        if (!LoadFunction("xrEnumerateDisplayRefreshRatesFB", enumerate) ||
            !LoadFunction("xrGetDisplayRefreshRateFB", get) ||
            !LoadFunction("xrRequestDisplayRefreshRateFB", request)) {
            return;
        }
        u32 count = 0;
        if (!Check(enumerate(session, 0, &count, nullptr), "xrEnumerateDisplayRefreshRatesFB")) {
            return;
        }
        std::vector<float> rates(count);
        enumerate(session, count, &count, rates.data());
        float current = 0.0f;
        get(session, &current);
        std::string listed;
        float best = 0.0f;
        for (const float rate : rates) {
            listed += fmt::format("{}{:.0f}", listed.empty() ? "" : ", ", rate);
            const float multiple = rate / 60.0f;
            if (std::abs(multiple - std::round(multiple)) < 0.01f && rate > best) {
                best = rate;
            }
        }
        LOG_INFO(Lib_Hmd, "OpenXR: display refresh {:.0f} Hz, available {}", current, listed);
        if (best == 0.0f) {
            LOG_WARNING(Lib_Hmd,
                        "OpenXR: no refresh rate is a multiple of 60 Hz; 60 fps will judder "
                        "(enable 120 Hz in the headset's PC VR settings)");
            return;
        }
        if (std::abs(best - current) > 0.5f &&
            Check(request(session, best), "xrRequestDisplayRefreshRateFB")) {
            LOG_INFO(Lib_Hmd, "OpenXR: requested {:.0f} Hz", best);
        }
    }

    bool Check(XrResult result, const char* what) const {
        if (XR_SUCCEEDED(result)) {
            return true;
        }
        char name[XR_MAX_RESULT_STRING_SIZE] = "?";
        if (instance != XR_NULL_HANDLE) {
            xrResultToString(instance, result, name);
        }
        LOG_ERROR(Lib_Hmd, "OpenXR: {} failed: {} ({})", what, name, static_cast<s32>(result));
        return false;
    }

    template <typename T>
    bool LoadFunction(const char* name, T& out) {
        return Check(
            xrGetInstanceProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&out)),
            name);
    }

    bool XrNow(XrTime& out) const {
        if (convert_time == nullptr) {
            return false;
        }
#ifdef _WIN32
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        return XR_SUCCEEDED(convert_time(instance, &counter, &out));
#else
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return XR_SUCCEEDED(convert_time(instance, &ts, &out));
#endif
    }

    // Maps a guest process time to runtime time. Without a clock conversion extension the last
    // frame's predicted display time is used as the anchor.
    bool GuestToXrTime(u64 guest_us, XrTime& out) const {
        const s64 guest_now = static_cast<s64>(Libraries::Kernel::sceKernelGetProcessTime());
        XrTime xr_now;
        if (XrNow(xr_now)) {
            out = xr_now + (static_cast<s64>(guest_us) - guest_now) * 1000;
            return out > 0;
        }
        if (frame_state.predictedDisplayTime == 0) {
            return false;
        }
        out = frame_state.predictedDisplayTime +
              (static_cast<s64>(guest_us) - static_cast<s64>(guest_time_at_wait)) * 1000;
        return out > 0;
    }

    XrPath Path(const char* path) const {
        XrPath out = XR_NULL_PATH;
        xrStringToPath(instance, path, &out);
        return out;
    }

    bool CreateAction(const char* name, XrActionType type, XrAction& out) {
        XrActionCreateInfo info{
            .type = XR_TYPE_ACTION_CREATE_INFO,
            .actionType = type,
            .countSubactionPaths = HandCount,
            .subactionPaths = hand_paths.data(),
        };
        std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(info.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        return Check(xrCreateAction(action_set, &info, &out), name);
    }

    // Suggests bindings for one interaction profile. Runtimes reject profiles they do not know
    // (XR_ERROR_PATH_UNSUPPORTED); that is fine, the others still apply.
    void SuggestBindings(const char* profile,
                         std::initializer_list<std::pair<XrAction, const char*>> bindings) {
        std::vector<XrActionSuggestedBinding> suggested;
        for (const auto& [action, path] : bindings) {
            suggested.push_back({action, Path(path)});
        }
        const XrInteractionProfileSuggestedBinding info{
            .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
            .interactionProfile = Path(profile),
            .countSuggestedBindings = static_cast<u32>(suggested.size()),
            .suggestedBindings = suggested.data(),
        };
        const XrResult result = xrSuggestInteractionProfileBindings(instance, &info);
        if (XR_FAILED(result)) {
            LOG_INFO(Lib_Hmd, "OpenXR: no bindings for {} ({})", profile, static_cast<s32>(result));
        }
    }

    // The DualShock 4 mapping: sticks, triggers (R2/L2), grips (R1/L1), A/B/X/Y as
    // Cross/Circle/Square/Triangle, stick clicks (L3/R3) and menu (Options).
    bool CreateActions() {
        hand_paths = {Path("/user/hand/left"), Path("/user/hand/right")};
        XrActionSetCreateInfo set_info{.type = XR_TYPE_ACTION_SET_CREATE_INFO, .priority = 0};
        std::strncpy(set_info.actionSetName, "psvr_pad", XR_MAX_ACTION_SET_NAME_SIZE - 1);
        std::strncpy(set_info.localizedActionSetName, "PSVR pad",
                     XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
        if (!Check(xrCreateActionSet(instance, &set_info, &action_set), "xrCreateActionSet")) {
            return false;
        }
        if (!CreateAction("stick", XR_ACTION_TYPE_VECTOR2F_INPUT, action_stick) ||
            !CreateAction("trigger", XR_ACTION_TYPE_FLOAT_INPUT, action_trigger) ||
            !CreateAction("squeeze", XR_ACTION_TYPE_FLOAT_INPUT, action_squeeze) ||
            !CreateAction("primary", XR_ACTION_TYPE_BOOLEAN_INPUT, action_primary) ||
            !CreateAction("secondary", XR_ACTION_TYPE_BOOLEAN_INPUT, action_secondary) ||
            !CreateAction("stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, action_stick_click) ||
            !CreateAction("menu", XR_ACTION_TYPE_BOOLEAN_INPUT, action_menu) ||
            !CreateAction("grip", XR_ACTION_TYPE_POSE_INPUT, action_grip)) {
            return false;
        }

        SuggestBindings("/interaction_profiles/oculus/touch_controller",
                        {{action_stick, "/user/hand/left/input/thumbstick"},
                         {action_stick, "/user/hand/right/input/thumbstick"},
                         {action_trigger, "/user/hand/left/input/trigger/value"},
                         {action_trigger, "/user/hand/right/input/trigger/value"},
                         {action_squeeze, "/user/hand/left/input/squeeze/value"},
                         {action_squeeze, "/user/hand/right/input/squeeze/value"},
                         {action_primary, "/user/hand/left/input/x/click"},
                         {action_primary, "/user/hand/right/input/a/click"},
                         {action_secondary, "/user/hand/left/input/y/click"},
                         {action_secondary, "/user/hand/right/input/b/click"},
                         {action_stick_click, "/user/hand/left/input/thumbstick/click"},
                         {action_stick_click, "/user/hand/right/input/thumbstick/click"},
                         {action_menu, "/user/hand/left/input/menu/click"},
                         {action_grip, "/user/hand/left/input/grip/pose"},
                         {action_grip, "/user/hand/right/input/grip/pose"}});
        SuggestBindings("/interaction_profiles/valve/index_controller",
                        {{action_stick, "/user/hand/left/input/thumbstick"},
                         {action_stick, "/user/hand/right/input/thumbstick"},
                         {action_trigger, "/user/hand/left/input/trigger/value"},
                         {action_trigger, "/user/hand/right/input/trigger/value"},
                         {action_squeeze, "/user/hand/left/input/squeeze/value"},
                         {action_squeeze, "/user/hand/right/input/squeeze/value"},
                         {action_primary, "/user/hand/left/input/a/click"},
                         {action_primary, "/user/hand/right/input/a/click"},
                         {action_secondary, "/user/hand/left/input/b/click"},
                         {action_secondary, "/user/hand/right/input/b/click"},
                         {action_stick_click, "/user/hand/left/input/thumbstick/click"},
                         {action_stick_click, "/user/hand/right/input/thumbstick/click"},
                         {action_grip, "/user/hand/left/input/grip/pose"},
                         {action_grip, "/user/hand/right/input/grip/pose"}});
        SuggestBindings("/interaction_profiles/htc/vive_controller",
                        {{action_stick, "/user/hand/left/input/trackpad"},
                         {action_stick, "/user/hand/right/input/trackpad"},
                         {action_trigger, "/user/hand/left/input/trigger/value"},
                         {action_trigger, "/user/hand/right/input/trigger/value"},
                         {action_squeeze, "/user/hand/left/input/squeeze/click"},
                         {action_squeeze, "/user/hand/right/input/squeeze/click"},
                         {action_primary, "/user/hand/left/input/trackpad/click"},
                         {action_primary, "/user/hand/right/input/trackpad/click"},
                         {action_menu, "/user/hand/left/input/menu/click"},
                         {action_secondary, "/user/hand/right/input/menu/click"},
                         {action_grip, "/user/hand/left/input/grip/pose"},
                         {action_grip, "/user/hand/right/input/grip/pose"}});
        SuggestBindings("/interaction_profiles/microsoft/motion_controller",
                        {{action_stick, "/user/hand/left/input/thumbstick"},
                         {action_stick, "/user/hand/right/input/thumbstick"},
                         {action_trigger, "/user/hand/left/input/trigger/value"},
                         {action_trigger, "/user/hand/right/input/trigger/value"},
                         {action_squeeze, "/user/hand/left/input/squeeze/click"},
                         {action_squeeze, "/user/hand/right/input/squeeze/click"},
                         {action_primary, "/user/hand/left/input/trackpad/click"},
                         {action_primary, "/user/hand/right/input/trackpad/click"},
                         {action_stick_click, "/user/hand/left/input/thumbstick/click"},
                         {action_stick_click, "/user/hand/right/input/thumbstick/click"},
                         {action_menu, "/user/hand/left/input/menu/click"},
                         {action_grip, "/user/hand/left/input/grip/pose"},
                         {action_grip, "/user/hand/right/input/grip/pose"}});
        SuggestBindings("/interaction_profiles/khr/simple_controller",
                        {{action_trigger, "/user/hand/left/input/select/click"},
                         {action_trigger, "/user/hand/right/input/select/click"},
                         {action_menu, "/user/hand/left/input/menu/click"},
                         {action_secondary, "/user/hand/right/input/menu/click"},
                         {action_grip, "/user/hand/left/input/grip/pose"},
                         {action_grip, "/user/hand/right/input/grip/pose"}});

        for (u32 hand = 0; hand < HandCount; hand++) {
            const XrActionSpaceCreateInfo space_info{
                .type = XR_TYPE_ACTION_SPACE_CREATE_INFO,
                .action = action_grip,
                .subactionPath = hand_paths[hand],
                .poseInActionSpace = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}},
            };
            if (!Check(xrCreateActionSpace(session, &space_info, &grip_spaces[hand]),
                       "xrCreateActionSpace")) {
                return false;
            }
        }
        const XrSessionActionSetsAttachInfo attach{
            .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
            .countActionSets = 1,
            .actionSets = &action_set,
        };
        actions_attached =
            Check(xrAttachSessionActionSets(session, &attach), "xrAttachSessionActionSets");
        return actions_attached;
    }

    void DestroyActions() {
        for (XrSpace& space : grip_spaces) {
            if (space != XR_NULL_HANDLE) {
                xrDestroySpace(space);
                space = XR_NULL_HANDLE;
            }
        }
        if (action_set != XR_NULL_HANDLE) {
            // Destroying the set destroys its actions.
            xrDestroyActionSet(action_set);
            action_set = XR_NULL_HANDLE;
        }
        action_stick = action_trigger = action_squeeze = action_primary = action_secondary =
            action_stick_click = action_menu = action_grip = XR_NULL_HANDLE;
        actions_attached = false;
    }

    bool GetBool(XrAction action, XrPath hand) const {
        const XrActionStateGetInfo info{
            .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = action, .subactionPath = hand};
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(xrGetActionStateBoolean(session, &info, &state)) && state.isActive &&
               state.currentState;
    }

    float GetFloat(XrAction action, XrPath hand) const {
        const XrActionStateGetInfo info{
            .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = action, .subactionPath = hand};
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_FAILED(xrGetActionStateFloat(session, &info, &state)) || !state.isActive) {
            return 0.0f;
        }
        return state.currentState;
    }

    bool CreateSwapchains() {
        u32 count = 0;
        if (!Check(xrEnumerateSwapchainFormats(session, 0, &count, nullptr),
                   "xrEnumerateSwapchainFormats")) {
            return false;
        }
        std::vector<s64> formats(count);
        xrEnumerateSwapchainFormats(session, count, &count, formats.data());

        VkFormat chosen = VK_FORMAT_UNDEFINED;
        for (const VkFormat preferred : PreferredFormats) {
            if (std::find(formats.begin(), formats.end(), static_cast<s64>(preferred)) !=
                formats.end()) {
                chosen = preferred;
                break;
            }
        }
        if (chosen == VK_FORMAT_UNDEFINED) {
            LOG_ERROR(Lib_Hmd, "OpenXR: runtime offers no 8-bit RGBA swapchain format");
            return false;
        }

        for (u32 eye = 0; eye < EyeCount; eye++) {
            auto& sc = swapchains[eye];
            sc.width = config_views[eye].recommendedImageRectWidth;
            sc.height = config_views[eye].recommendedImageRectHeight;
            sc.format = chosen;
            const XrSwapchainCreateInfo info{
                .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
                .usageFlags =
                    XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
                .format = static_cast<s64>(chosen),
                .sampleCount = 1,
                .width = sc.width,
                .height = sc.height,
                .faceCount = 1,
                .arraySize = 1,
                .mipCount = 1,
            };
            if (!Check(xrCreateSwapchain(session, &info, &sc.handle), "xrCreateSwapchain")) {
                return false;
            }
            u32 image_count = 0;
            xrEnumerateSwapchainImages(sc.handle, 0, &image_count, nullptr);
            sc.images.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
            if (!Check(xrEnumerateSwapchainImages(
                           sc.handle, image_count, &image_count,
                           reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data())),
                       "xrEnumerateSwapchainImages")) {
                return false;
            }
        }
        LOG_INFO(Lib_Hmd, "OpenXR: eye swapchains {}x{} / {}x{}, VkFormat {}", swapchains[0].width,
                 swapchains[0].height, swapchains[1].width, swapchains[1].height,
                 static_cast<s32>(chosen));
        return true;
    }

    void DestroySwapchains() {
        for (auto& sc : swapchains) {
            if (sc.handle != XR_NULL_HANDLE) {
                xrDestroySwapchain(sc.handle);
            }
            sc = {};
        }
    }

    void HandleSessionState(XrSessionState new_state) {
        LOG_INFO(Lib_Hmd, "OpenXR: session {} -> {}", SessionStateName(state),
                 SessionStateName(new_state));
        state = new_state;
        switch (new_state) {
        case XR_SESSION_STATE_READY: {
            const XrSessionBeginInfo begin{
                .type = XR_TYPE_SESSION_BEGIN_INFO,
                .primaryViewConfigurationType = ViewConfig,
            };
            if (Check(xrBeginSession(session, &begin), "xrBeginSession")) {
                running = true;
                if (swapchains[0].handle == XR_NULL_HANDLE && !CreateSwapchains()) {
                    DestroySwapchains();
                }
            }
            break;
        }
        case XR_SESSION_STATE_STOPPING:
            running = false;
            Check(xrEndSession(session), "xrEndSession");
            break;
        case XR_SESSION_STATE_EXITING:
        case XR_SESSION_STATE_LOSS_PENDING:
            running = false;
            break;
        default:
            break;
        }
    }

    bool LocateViews(XrTime time, std::array<XrView, EyeCount>& views) {
        const XrViewLocateInfo info{
            .type = XR_TYPE_VIEW_LOCATE_INFO,
            .viewConfigurationType = ViewConfig,
            .displayTime = time,
            .space = app_space,
        };
        XrViewState view_state{XR_TYPE_VIEW_STATE};
        views.fill({XR_TYPE_VIEW});
        u32 count = 0;
        if (XR_FAILED(xrLocateViews(session, &info, &view_state, EyeCount, &count, views.data())) ||
            count != EyeCount ||
            (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
            return false;
        }
        for (u32 eye = 0; eye < EyeCount; eye++) {
            eye_fov[eye] = FromXr(views[eye].fov);
        }
        have_fov = true;
        const XrVector3f& l = views[EyeLeft].pose.position;
        const XrVector3f& r = views[EyeRight].pose.position;
        const float dist = std::sqrt((l.x - r.x) * (l.x - r.x) + (l.y - r.y) * (l.y - r.y) +
                                     (l.z - r.z) * (l.z - r.z));
        if (dist > 0.04f && dist < 0.09f) {
            ipd = dist;
        }
        return true;
    }
};

OpenXrRuntime::OpenXrRuntime() : impl{std::make_unique<Impl>()} {}

OpenXrRuntime::~OpenXrRuntime() {
    DestroySession();
    if (impl->instance != XR_NULL_HANDLE) {
        xrDestroyInstance(impl->instance);
    }
}

bool OpenXrRuntime::Initialize() {
    auto& d = *impl;

    u32 ext_count = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr))) {
        LOG_WARNING(Lib_Hmd, "OpenXR: no runtime available (is one installed and active?)");
        return false;
    }
    std::vector<XrExtensionProperties> exts(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, exts.data());
    const auto has_ext = [&](const char* name) {
        return std::any_of(exts.begin(), exts.end(),
                           [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; });
    };

    if (!has_ext(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) {
        LOG_ERROR(Lib_Hmd, "OpenXR: runtime lacks {}", XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
        return false;
    }
    std::vector<const char*> enabled{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
#ifdef _WIN32
    const char* time_ext = XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME;
#else
    const char* time_ext = XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME;
#endif
    const bool has_time_ext = has_ext(time_ext);
    if (has_time_ext) {
        enabled.push_back(time_ext);
    } else {
        LOG_WARNING(Lib_Hmd, "OpenXR: runtime lacks {}, pose timing will be approximate", time_ext);
    }
    d.has_refresh_rate_ext = has_ext(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (d.has_refresh_rate_ext) {
        enabled.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    }
#ifdef _WIN32
    const bool has_audio_ext = has_ext(XR_OCULUS_AUDIO_DEVICE_GUID_EXTENSION_NAME);
    if (has_audio_ext) {
        enabled.push_back(XR_OCULUS_AUDIO_DEVICE_GUID_EXTENSION_NAME);
    }
#endif

    XrInstanceCreateInfo create_info{
        .type = XR_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = static_cast<u32>(enabled.size()),
        .enabledExtensionNames = enabled.data(),
    };
    std::strncpy(create_info.applicationInfo.applicationName, "shadPS4",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    create_info.applicationInfo.applicationVersion = 1;
    std::strncpy(create_info.applicationInfo.engineName, "shadPS4", XR_MAX_ENGINE_NAME_SIZE - 1);
    create_info.applicationInfo.engineVersion = 1;
    create_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    if (!d.Check(xrCreateInstance(&create_info, &d.instance), "xrCreateInstance")) {
        return false;
    }

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(d.instance, &props);
    LOG_INFO(Lib_Hmd, "OpenXR: runtime {} {}.{}.{}", props.runtimeName,
             XR_VERSION_MAJOR(props.runtimeVersion), XR_VERSION_MINOR(props.runtimeVersion),
             XR_VERSION_PATCH(props.runtimeVersion));

    const XrSystemGetInfo system_info{
        .type = XR_TYPE_SYSTEM_GET_INFO,
        .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
    };
    if (!d.Check(xrGetSystem(d.instance, &system_info, &d.system), "xrGetSystem (headset)")) {
        return false;
    }
    XrSystemProperties system_props{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(d.instance, d.system, &system_props);
    LOG_INFO(Lib_Hmd, "OpenXR: system '{}'", system_props.systemName);

    u32 view_count = 0;
    xrEnumerateViewConfigurationViews(d.instance, d.system, ViewConfig, 0, &view_count, nullptr);
    if (view_count != EyeCount) {
        LOG_ERROR(Lib_Hmd, "OpenXR: expected 2 stereo views, runtime reports {}", view_count);
        return false;
    }
    d.config_views.fill({XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(d.instance, d.system, ViewConfig, view_count, &view_count,
                                      d.config_views.data());

    if (!d.LoadFunction("xrGetVulkanGraphicsRequirements2KHR", d.get_graphics_requirements) ||
        !d.LoadFunction("xrCreateVulkanInstanceKHR", d.create_vulkan_instance) ||
        !d.LoadFunction("xrGetVulkanGraphicsDevice2KHR", d.get_vulkan_device) ||
        !d.LoadFunction("xrCreateVulkanDeviceKHR", d.create_vulkan_device)) {
        return false;
    }
    if (has_time_ext) {
#ifdef _WIN32
        d.LoadFunction("xrConvertWin32PerformanceCounterToTimeKHR", d.convert_time);
#else
        d.LoadFunction("xrConvertTimespecTimeToTimeKHR", d.convert_time);
#endif
    }
#ifdef _WIN32
    // The headset's own audio device. The Windows default output is often something else (a
    // DualSense's speaker, the desk speakers), so the game would play where the player is not.
    PFN_xrGetAudioOutputDeviceGuidOculus get_audio_output = nullptr;
    wchar_t audio_id[XR_MAX_AUDIO_DEVICE_STR_SIZE_OCULUS]{};
    if (has_audio_ext && d.LoadFunction("xrGetAudioOutputDeviceGuidOculus", get_audio_output) &&
        d.Check(get_audio_output(d.instance, audio_id), "xrGetAudioOutputDeviceGuidOculus")) {
        d.audio_output_device = AudioEndpointName(audio_id);
        LOG_INFO(Lib_Hmd, "OpenXR: headset audio device '{}'", d.audio_output_device);
    }
#endif
    return true;
}

const std::string& OpenXrRuntime::GetAudioOutputDevice() const {
    return impl->audio_output_device;
}

VkResult OpenXrRuntime::CreateVulkanInstance(const VkInstanceCreateInfo* create_info,
                                             PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                                             VkInstance* out_instance) {
    auto& d = *impl;
    // The spec requires the requirements query before instance or device creation.
    XrGraphicsRequirementsVulkan2KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (!d.Check(d.get_graphics_requirements(d.instance, d.system, &reqs),
                 "xrGetVulkanGraphicsRequirements2KHR")) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    LOG_INFO(Lib_Hmd, "OpenXR: runtime supports Vulkan {}.{} to {}.{}",
             XR_VERSION_MAJOR(reqs.minApiVersionSupported),
             XR_VERSION_MINOR(reqs.minApiVersionSupported),
             XR_VERSION_MAJOR(reqs.maxApiVersionSupported),
             XR_VERSION_MINOR(reqs.maxApiVersionSupported));

    const XrVulkanInstanceCreateInfoKHR info{
        .type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR,
        .systemId = d.system,
        .pfnGetInstanceProcAddr = get_instance_proc_addr,
        .vulkanCreateInfo = create_info,
    };
    VkResult vk_result = VK_SUCCESS;
    if (!d.Check(d.create_vulkan_instance(d.instance, &info, out_instance, &vk_result),
                 "xrCreateVulkanInstanceKHR")) {
        return vk_result != VK_SUCCESS ? vk_result : VK_ERROR_INITIALIZATION_FAILED;
    }
    return vk_result;
}

VkPhysicalDevice OpenXrRuntime::GetVulkanPhysicalDevice(VkInstance instance) {
    auto& d = *impl;
    const XrVulkanGraphicsDeviceGetInfoKHR info{
        .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR,
        .systemId = d.system,
        .vulkanInstance = instance,
    };
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    d.Check(d.get_vulkan_device(d.instance, &info, &physical_device),
            "xrGetVulkanGraphicsDevice2KHR");
    return physical_device;
}

VkResult OpenXrRuntime::CreateVulkanDevice(VkPhysicalDevice physical_device,
                                           const VkDeviceCreateInfo* create_info,
                                           PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                                           VkDevice* out_device) {
    auto& d = *impl;
    const XrVulkanDeviceCreateInfoKHR info{
        .type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR,
        .systemId = d.system,
        .pfnGetInstanceProcAddr = get_instance_proc_addr,
        .vulkanPhysicalDevice = physical_device,
        .vulkanCreateInfo = create_info,
    };
    VkResult vk_result = VK_SUCCESS;
    if (!d.Check(d.create_vulkan_device(d.instance, &info, out_device, &vk_result),
                 "xrCreateVulkanDeviceKHR")) {
        return vk_result != VK_SUCCESS ? vk_result : VK_ERROR_INITIALIZATION_FAILED;
    }
    return vk_result;
}

bool OpenXrRuntime::CreateSession(VkInstance instance, VkPhysicalDevice physical_device,
                                  VkDevice device, u32 queue_family_index, u32 queue_index) {
    auto& d = *impl;
    std::scoped_lock lk{d.mutex};
    const XrGraphicsBindingVulkan2KHR binding{
        .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
        .instance = instance,
        .physicalDevice = physical_device,
        .device = device,
        .queueFamilyIndex = queue_family_index,
        .queueIndex = queue_index,
    };
    const XrSessionCreateInfo info{
        .type = XR_TYPE_SESSION_CREATE_INFO,
        .next = &binding,
        .systemId = d.system,
    };
    if (!d.Check(xrCreateSession(d.instance, &info, &d.session), "xrCreateSession")) {
        return false;
    }

    XrReferenceSpaceCreateInfo space_info{
        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
        .poseInReferenceSpace = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}},
    };
    if (!d.Check(xrCreateReferenceSpace(d.session, &space_info, &d.local_space), "LOCAL space") ||
        !d.Check(xrCreateReferenceSpace(d.session, &space_info, &d.app_space), "app space")) {
        return false;
    }
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!d.Check(xrCreateReferenceSpace(d.session, &space_info, &d.view_space), "VIEW space")) {
        return false;
    }
    LOG_INFO(Lib_Hmd, "OpenXR: session created, recommended eye size {}x{}",
             d.config_views[0].recommendedImageRectWidth,
             d.config_views[0].recommendedImageRectHeight);
    d.RequestRefreshRate();
    if (!d.CreateActions()) {
        // Input is optional: the PC gamepad still works.
        LOG_WARNING(Lib_Hmd, "OpenXR: controller input unavailable");
        d.DestroyActions();
    }
    return true;
}

void OpenXrRuntime::DestroySession() {
    auto& d = *impl;
    std::scoped_lock lk{d.mutex};
    if (d.session == XR_NULL_HANDLE) {
        return;
    }
    if (d.running) {
        xrRequestExitSession(d.session);
        xrEndSession(d.session);
        d.running = false;
    }
    d.pending_frames.clear();
    d.DestroyActions();
    d.DestroySwapchains();
    for (XrSpace* space : {&d.app_space, &d.view_space, &d.local_space}) {
        if (*space != XR_NULL_HANDLE) {
            xrDestroySpace(*space);
            *space = XR_NULL_HANDLE;
        }
    }
    xrDestroySession(d.session);
    d.session = XR_NULL_HANDLE;
}

void OpenXrRuntime::PollEvents() {
    auto& d = *impl;
    std::scoped_lock lk{d.mutex};
    if (d.instance == XR_NULL_HANDLE || d.instance_lost) {
        return;
    }
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(d.instance, &event) == XR_SUCCESS) {
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto& changed = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
            if (changed.session == d.session) {
                d.HandleSessionState(changed.state);
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            LOG_ERROR(Lib_Hmd, "OpenXR: runtime is going away; VR output stops");
            d.instance_lost = true;
            d.running = false;
            break;
        default:
            break;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

bool OpenXrRuntime::IsSessionRunning() const {
    std::scoped_lock lk{impl->mutex};
    return impl->running && impl->swapchains[0].handle != XR_NULL_HANDLE;
}

bool OpenXrRuntime::WaitFrame() {
    auto& d = *impl;
    XrSession session;
    {
        std::scoped_lock lk{d.mutex};
        if (!d.running || d.swapchains[0].handle == XR_NULL_HANDLE ||
            d.pending_frames.size() >= Impl::MaxPendingFrames) {
            return false;
        }
        session = d.session;
    }
    // Outside the lock: this blocks until the runtime wants the next frame, and until the
    // previous frame has been begun on the GPU thread.
    const XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    if (!d.Check(xrWaitFrame(session, &wait_info, &frame_state), "xrWaitFrame")) {
        return false;
    }
    std::scoped_lock lk{d.mutex};
    d.pending_frames.push_back({frame_state, Libraries::Kernel::sceKernelGetProcessTime()});
    return true;
}

void OpenXrRuntime::DiscardPendingFrame() {
    auto& d = *impl;
    std::scoped_lock lk{d.mutex};
    if (d.pending_frames.empty()) {
        return;
    }
    const Impl::PendingFrame pending = d.pending_frames.front();
    d.pending_frames.pop_front();
    if (!d.running) {
        return;
    }
    const XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    if (!d.Check(xrBeginFrame(d.session, &begin_info), "xrBeginFrame (discard)")) {
        return;
    }
    const XrFrameEndInfo end_info{
        .type = XR_TYPE_FRAME_END_INFO,
        .displayTime = pending.state.predictedDisplayTime,
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        .layerCount = 0,
        .layers = nullptr,
    };
    d.Check(xrEndFrame(d.session, &end_info), "xrEndFrame (discard)");
}

bool OpenXrRuntime::BeginFrame(std::array<EyeTarget, EyeCount>& targets, bool& should_render) {
    auto& d = *impl;
    should_render = false;
    XrSession session;
    std::optional<Impl::PendingFrame> pending;
    {
        std::scoped_lock lk{d.mutex};
        // A frame WaitFrame paced for us belongs to this BeginFrame whether or not the session
        // is still usable, so it is taken off the queue first.
        if (!d.pending_frames.empty()) {
            pending = d.pending_frames.front();
            d.pending_frames.pop_front();
        }
        if (!d.running || d.swapchains[0].handle == XR_NULL_HANDLE) {
            return false;
        }
        session = d.session;
    }

    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    u64 guest_time_at_wait;
    if (pending) {
        frame_state = pending->state;
        guest_time_at_wait = pending->guest_time_at_wait;
    } else {
        // Nobody paced this frame ahead of time: wait here, outside the lock so pose queries
        // from game threads are not held up.
        const XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
        if (!d.Check(xrWaitFrame(session, &wait_info, &frame_state), "xrWaitFrame")) {
            return false;
        }
        guest_time_at_wait = Libraries::Kernel::sceKernelGetProcessTime();
    }

    std::scoped_lock lk{d.mutex};
    d.frame_state = frame_state;
    d.guest_time_at_wait = guest_time_at_wait;
    const XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    if (!d.Check(xrBeginFrame(session, &begin_info), "xrBeginFrame")) {
        return false;
    }
    if (!frame_state.shouldRender) {
        return true;
    }

    for (u32 eye = 0; eye < EyeCount; eye++) {
        auto& sc = d.swapchains[eye];
        const XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (!d.Check(xrAcquireSwapchainImage(sc.handle, &acquire_info, &sc.acquired_index),
                     "xrAcquireSwapchainImage")) {
            return true;
        }
        sc.acquired = true;
        const XrSwapchainImageWaitInfo image_wait{
            .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
            .timeout = XR_INFINITE_DURATION,
        };
        if (!d.Check(xrWaitSwapchainImage(sc.handle, &image_wait), "xrWaitSwapchainImage")) {
            return true;
        }
        targets[eye] = {sc.images[sc.acquired_index].image, sc.width, sc.height, sc.format};
    }
    should_render = true;
    return true;
}

void OpenXrRuntime::EndFrame(bool rendered, const std::array<EyeView, EyeCount>& views) {
    auto& d = *impl;
    std::scoped_lock lk{d.mutex};

    bool all_released = true;
    for (auto& sc : d.swapchains) {
        if (sc.acquired) {
            const XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            all_released &= d.Check(xrReleaseSwapchainImage(sc.handle, &release_info),
                                    "xrReleaseSwapchainImage");
            sc.acquired = false;
        } else {
            all_released = false;
        }
    }

    std::array<XrCompositionLayerProjectionView, EyeCount> proj_views{};
    for (u32 eye = 0; eye < EyeCount; eye++) {
        const auto& sc = d.swapchains[eye];
        proj_views[eye] = {
            .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
            .pose = ToXr(views[eye].pose),
            .fov = ToXr(views[eye].fov),
            .subImage = {.swapchain = sc.handle,
                         .imageRect = {{0, 0},
                                       {static_cast<s32>(sc.width), static_cast<s32>(sc.height)}},
                         .imageArrayIndex = 0},
        };
    }
    const XrCompositionLayerProjection layer{
        .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
        .space = d.app_space,
        .viewCount = EyeCount,
        .views = proj_views.data(),
    };
    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
    const bool submit_layer = rendered && all_released && d.frame_state.shouldRender;
    const XrFrameEndInfo end_info{
        .type = XR_TYPE_FRAME_END_INFO,
        .displayTime = d.frame_state.predictedDisplayTime,
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        .layerCount = submit_layer ? 1u : 0u,
        .layers = submit_layer ? layers : nullptr,
    };
    d.Check(xrEndFrame(d.session, &end_info), "xrEndFrame");
}

bool OpenXrRuntime::Sample(u64 guest_time_us, TrackingSample& out) {
    auto& d = *impl;
    std::scoped_lock lk{d.mutex};
    if (d.session == XR_NULL_HANDLE || d.instance_lost) {
        return false;
    }
    XrTime time;
    if (!d.GuestToXrTime(guest_time_us, time)) {
        return false;
    }

    std::array<XrView, EyeCount> views;
    if (!d.LocateViews(time, views)) {
        return false;
    }

    XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION, &velocity};
    if (XR_FAILED(xrLocateSpace(d.view_space, d.app_space, time, &head)) ||
        (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0) {
        return false;
    }

    out = {};
    out.valid = true;
    out.guest_time_us = guest_time_us;
    out.head = FromXr(head.pose);
    out.eyes[EyeLeft] = FromXr(views[EyeLeft].pose);
    out.eyes[EyeRight] = FromXr(views[EyeRight].pose);
    if (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) {
        out.linear_velocity = {velocity.linearVelocity.x, velocity.linearVelocity.y,
                               velocity.linearVelocity.z};
    }
    if (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) {
        out.angular_velocity = {velocity.angularVelocity.x, velocity.angularVelocity.y,
                                velocity.angularVelocity.z};
    }
    return true;
}

bool OpenXrRuntime::SyncInput(ControllerInput& out) {
    auto& d = *impl;
    std::scoped_lock lk{d.mutex};
    if (!d.actions_attached || d.state != XR_SESSION_STATE_FOCUSED) {
        return false;
    }
    const XrActiveActionSet active{.actionSet = d.action_set, .subactionPath = XR_NULL_PATH};
    const XrActionsSyncInfo sync{
        .type = XR_TYPE_ACTIONS_SYNC_INFO,
        .countActiveActionSets = 1,
        .activeActionSets = &active,
    };
    // XR_SESSION_NOT_FOCUSED is a success code that means "no input right now".
    if (xrSyncActions(d.session, &sync) != XR_SUCCESS) {
        return false;
    }
    for (u32 hand = 0; hand < HandCount; hand++) {
        const XrPath path = d.hand_paths[hand];
        HandInput& in = out.hands[hand];
        const XrActionStateGetInfo info{
            .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = d.action_stick, .subactionPath = path};
        XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
        const bool have_stick =
            XR_SUCCEEDED(xrGetActionStateVector2f(d.session, &info, &stick)) && stick.isActive;
        XrActionStateGetInfo pose_info = info;
        pose_info.action = d.action_grip;
        XrActionStatePose pose{XR_TYPE_ACTION_STATE_POSE};
        const bool have_pose =
            XR_SUCCEEDED(xrGetActionStatePose(d.session, &pose_info, &pose)) && pose.isActive;
        in.active = have_stick || have_pose;
        in.stick_x = have_stick ? stick.currentState.x : 0.0f;
        in.stick_y = have_stick ? stick.currentState.y : 0.0f;
        in.trigger = d.GetFloat(d.action_trigger, path);
        in.squeeze = d.GetFloat(d.action_squeeze, path);
        in.primary = d.GetBool(d.action_primary, path);
        in.secondary = d.GetBool(d.action_secondary, path);
        in.stick_click = d.GetBool(d.action_stick_click, path);
        in.menu = d.GetBool(d.action_menu, path);
    }
    return true;
}

bool OpenXrRuntime::LocateHand(Hand hand, u64 guest_time_us, Pose& out) {
    auto& d = *impl;
    std::scoped_lock lk{d.mutex};
    if (!d.actions_attached || d.grip_spaces[hand] == XR_NULL_HANDLE ||
        d.session == XR_NULL_HANDLE) {
        return false;
    }
    XrTime time;
    if (!d.GuestToXrTime(guest_time_us, time)) {
        return false;
    }
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    constexpr XrSpaceLocationFlags Needed =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if (XR_FAILED(xrLocateSpace(d.grip_spaces[hand], d.app_space, time, &location)) ||
        (location.locationFlags & Needed) != Needed) {
        return false;
    }
    out = FromXr(location.pose);
    return true;
}

void OpenXrRuntime::Recenter() {
    auto& d = *impl;
    std::scoped_lock lk{d.mutex};
    if (d.session == XR_NULL_HANDLE) {
        return;
    }
    XrTime time;
    if (!d.XrNow(time)) {
        time = d.frame_state.predictedDisplayTime;
    }
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    if (time == 0 || XR_FAILED(xrLocateSpace(d.view_space, d.local_space, time, &head)) ||
        (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0) {
        LOG_WARNING(Lib_Hmd, "OpenXR: cannot recenter, head pose unknown");
        return;
    }
    const Pose origin{YawOnly(FromXr(head.pose).orientation), FromXr(head.pose).position};
    const XrReferenceSpaceCreateInfo space_info{
        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
        .poseInReferenceSpace = ToXr(origin),
    };
    XrSpace new_space;
    if (d.Check(xrCreateReferenceSpace(d.session, &space_info, &new_space), "recenter space")) {
        xrDestroySpace(d.app_space);
        d.app_space = new_space;
        LOG_INFO(Lib_Hmd, "OpenXR: recentered");
    }
}

FovTangents OpenXrRuntime::GetEyeFov(Eye eye) const {
    std::scoped_lock lk{impl->mutex};
    if (!impl->have_fov) {
        return PoseSource::GetEyeFov(eye);
    }
    return impl->eye_fov[eye];
}

float OpenXrRuntime::GetIpd() const {
    std::scoped_lock lk{impl->mutex};
    return impl->ipd;
}

} // namespace VR
