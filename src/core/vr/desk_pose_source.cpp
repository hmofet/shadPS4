// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <numbers>
#include <SDL3/SDL_keyboard.h>

#include "core/libraries/kernel/time.h"
#include "core/vr/pose_source.h"

namespace VR {

namespace {

constexpr float TurnRate = std::numbers::pi_v<float> / 2.0f; // 90 degrees per second
constexpr float MoveRate = 0.5f;                             // metres per second
constexpr float MaxPitch = std::numbers::pi_v<float> * 0.45f;

bool KeyDown(const bool* keys, SDL_Scancode code) {
    return keys != nullptr && keys[code];
}

} // namespace

DeskPoseSource::DeskPoseSource(bool use_keyboard_) : use_keyboard{use_keyboard_} {}

bool DeskPoseSource::Sample(u64 guest_time_us, TrackingSample& out) {
    const u64 now_us = Libraries::Kernel::sceKernelGetProcessTime();
    const float dt =
        last_update_us == 0 ? 0.0f : static_cast<float>(now_us - last_update_us) * 1e-6f;
    last_update_us = now_us;

    // Numpad: 4/6 yaw, 8/2 pitch, 7/9 roll, 1/3 strafe, +/- forward and back.
    // Recentering is handled by the service through the configurable recenter key.
    if (use_keyboard && dt > 0.0f && dt < 0.25f) {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        const auto axis = [&](SDL_Scancode neg, SDL_Scancode pos) {
            return (KeyDown(keys, pos) ? 1.0f : 0.0f) - (KeyDown(keys, neg) ? 1.0f : 0.0f);
        };
        yaw += axis(SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_4) * TurnRate * dt;
        pitch += axis(SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_8) * TurnRate * dt;
        pitch = std::clamp(pitch, -MaxPitch, MaxPitch);
        roll += axis(SDL_SCANCODE_KP_9, SDL_SCANCODE_KP_7) * TurnRate * dt;

        const Quat heading = FromAxisAngle({0.0f, 1.0f, 0.0f}, yaw);
        const Vec3 move{axis(SDL_SCANCODE_KP_1, SDL_SCANCODE_KP_3), 0.0f,
                        axis(SDL_SCANCODE_KP_PLUS, SDL_SCANCODE_KP_MINUS)};
        position = position + Rotate(heading, move) * (MoveRate * dt);
    }

    const Quat orientation = FromYawPitchRoll(yaw, pitch, roll);

    out = {};
    out.valid = true;
    out.guest_time_us = guest_time_us;
    out.head = {orientation, position};
    const float half_ipd = GetIpd() * 0.5f;
    out.eyes[EyeLeft] = Compose(out.head, {{}, {-half_ipd, 0.0f, 0.0f}});
    out.eyes[EyeRight] = Compose(out.head, {{}, {half_ipd, 0.0f, 0.0f}});

    // Angular velocity from the change since the previous sample, in tracking space.
    if (dt > 0.0f) {
        const Quat delta = Normalize(orientation * Conjugate(last_orientation));
        const float angle = 2.0f * std::acos(std::clamp(delta.w, -1.0f, 1.0f));
        const float s = std::sqrt(std::max(0.0f, 1.0f - delta.w * delta.w));
        if (s > 1e-6f) {
            const float rate = angle / dt;
            out.angular_velocity = {delta.x / s * rate, delta.y / s * rate, delta.z / s * rate};
        }
    }
    last_orientation = orientation;
    return true;
}

void DeskPoseSource::Recenter() {
    yaw = 0.0f;
    pitch = 0.0f;
    roll = 0.0f;
    position = {};
}

} // namespace VR
