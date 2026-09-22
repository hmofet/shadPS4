// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cmath>
#include "core/vr/vr_types.h"

// What a game hands libSceHmdReprojection with each frame, decoded from the sceHmdReprojectionStart
// parameters. Header only so the decoding can be unit tested without the emulator.
//
// Layout observed in WipEout Omega Collection (docs/psvr-openxr/IMPLEMENTATION.md, "Headset
// test"):
//
//   arg0 + 0x00  pointer to the left eye texture descriptor (T#)
//   arg0 + 0x08  pointer to the right eye texture descriptor
//   arg0 + 0x18  left eye projection:  { 1 / (tan_l + tan_r), 1 / (tan_u + tan_d),
//                                        tan_l / (tan_l + tan_r), tan_u / (tan_u + tan_d) }
//   arg0 + 0x28  right eye projection, same form
//   arg1 + 0x00  head pose the frame was rendered with, in PSVR camera space:
//                position xyz, orientation xyzw, then a u64 timestamp at +0x20
//
// The projection quadruples are exactly what a game derives from the sceHmdGetFieldOfView tangents
// (the scale and centre of the projection in texture space, measured from the top left), so the
// FOV the game really rendered with can be read back from them.

namespace VR {

struct StartEyeProjection {
    float scale_x = 0.0f;  // 1 / (tan_left + tan_right)
    float scale_y = 0.0f;  // 1 / (tan_up + tan_down)
    float center_x = 0.0f; // tan_left / (tan_left + tan_right), from the left edge
    float center_y = 0.0f; // tan_up / (tan_up + tan_down), from the top edge
};
static_assert(sizeof(StartEyeProjection) == 16);

struct StartRenderPose {
    float position_x = 0.0f;
    float position_y = 0.0f;
    float position_z = 0.0f;
    float orientation_x = 0.0f;
    float orientation_y = 0.0f;
    float orientation_z = 0.0f;
    float orientation_w = 1.0f;
    u32 reserved = 0;
    u64 timestamp = 0; // guest process time (microseconds)
};
static_assert(sizeof(StartRenderPose) == 40);

constexpr u64 StartProjectionOffset = 0x18;
constexpr u64 StartEyePointersSize = 2 * sizeof(u64);

// Decoded frame information. Parts that were missing or implausible are left invalid, and the
// service fills them from the last sample it gave the game.
struct HmdFrameInfo {
    bool have_pose = false;
    Pose head{}; // tracking space (the PSVR camera offset removed)
    u64 guest_time_us = 0;
    bool have_fov = false;
    std::array<FovTangents, EyeCount> fov{};
};

inline bool IsFinite(float v) {
    return std::isfinite(v);
}

// Turns a projection quadruple back into the four frustum tangents. False if the values are not a
// plausible projection (the layout may differ in other titles).
inline bool ProjectionToFov(const StartEyeProjection& p, FovTangents& out) {
    if (!IsFinite(p.scale_x) || !IsFinite(p.scale_y) || !IsFinite(p.center_x) ||
        !IsFinite(p.center_y)) {
        return false;
    }
    // A scale of 0.5 is a 90 degree frustum; 0.1 would be a 157 degree one, 2.0 a 28 degree one.
    if (p.scale_x < 0.1f || p.scale_x > 2.0f || p.scale_y < 0.1f || p.scale_y > 2.0f) {
        return false;
    }
    if (p.center_x <= 0.0f || p.center_x >= 1.0f || p.center_y <= 0.0f || p.center_y >= 1.0f) {
        return false;
    }
    out.left = p.center_x / p.scale_x;
    out.right = (1.0f - p.center_x) / p.scale_x;
    out.up = p.center_y / p.scale_y;
    out.down = (1.0f - p.center_y) / p.scale_y;
    return true;
}

// Reads the render pose, moving it from PSVR camera space back to tracking space. False if the
// values are not a plausible pose.
inline bool DecodeRenderPose(const StartRenderPose& p, float camera_distance, Pose& out,
                             u64& timestamp) {
    const float values[] = {p.position_x,    p.position_y,    p.position_z,   p.orientation_x,
                            p.orientation_y, p.orientation_z, p.orientation_w};
    for (const float v : values) {
        if (!IsFinite(v)) {
            return false;
        }
    }
    const float norm = p.orientation_x * p.orientation_x + p.orientation_y * p.orientation_y +
                       p.orientation_z * p.orientation_z + p.orientation_w * p.orientation_w;
    if (norm < 0.98f || norm > 1.02f) {
        return false;
    }
    // Room scale: anything beyond this is not a head position.
    constexpr float MaxDistance = 20.0f;
    if (std::abs(p.position_x) > MaxDistance || std::abs(p.position_y) > MaxDistance ||
        std::abs(p.position_z) > MaxDistance) {
        return false;
    }
    out.orientation =
        Normalize({p.orientation_x, p.orientation_y, p.orientation_z, p.orientation_w});
    out.position = {p.position_x, p.position_y, p.position_z - camera_distance};
    timestamp = p.timestamp;
    return true;
}

// True when two head poses are the same sample (the game copies the tracker result verbatim, so
// equality is exact up to float rounding).
inline bool SamePose(const Pose& a, const Pose& b) {
    constexpr float PositionEps = 1e-4f;
    constexpr float OrientationEps = 1e-4f;
    const Vec3 d = a.position - b.position;
    if (std::abs(d.x) > PositionEps || std::abs(d.y) > PositionEps || std::abs(d.z) > PositionEps) {
        return false;
    }
    const Quat qa = Normalize(a.orientation);
    const Quat qb = Normalize(b.orientation);
    const float dot = qa.x * qb.x + qa.y * qb.y + qa.z * qb.z + qa.w * qb.w;
    return std::abs(dot) > 1.0f - OrientationEps;
}

} // namespace VR
