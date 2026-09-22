// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cmath>
#include "common/types.h"

namespace VR {

// Tracking space conventions used throughout the VR code: right-handed, +Y up, -Z forward,
// metres. This matches both OpenXR and the PSVR tracker, so no axis flips are needed between them.

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct Pose {
    Quat orientation{};
    Vec3 position{};
};

// Tangents of the four half-angles of an eye frustum, all positive.
struct FovTangents {
    float left = 0.0f;
    float right = 0.0f;
    float up = 0.0f;
    float down = 0.0f;
};

enum Eye : u32 {
    EyeLeft = 0,
    EyeRight = 1,
    EyeCount = 2,
};

// What the renderer used for one eye: submitted back to the runtime with the image so it can
// reproject from the right place.
struct EyeView {
    Pose pose{};
    FovTangents fov{};
};
using HmdFrameViews = std::array<EyeView, EyeCount>;

struct TrackingSample {
    bool valid = false;
    Pose head{};
    Pose eyes[EyeCount]{};
    Vec3 linear_velocity{};
    Vec3 angular_velocity{};
    // Guest process time (microseconds) the sample was predicted for.
    u64 guest_time_us = 0;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 operator-(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 operator*(const Vec3& a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}

inline Quat operator*(const Quat& a, const Quat& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

inline Quat Conjugate(const Quat& q) {
    return {-q.x, -q.y, -q.z, q.w};
}

inline Quat Normalize(const Quat& q) {
    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len <= 0.0f) {
        return {};
    }
    return {q.x / len, q.y / len, q.z / len, q.w / len};
}

inline Vec3 Rotate(const Quat& q, const Vec3& v) {
    const Quat p{v.x, v.y, v.z, 0.0f};
    const Quat r = q * p * Conjugate(q);
    return {r.x, r.y, r.z};
}

inline Quat FromAxisAngle(const Vec3& axis, float radians) {
    const float s = std::sin(radians * 0.5f);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(radians * 0.5f)};
}

// Yaw about +Y, then pitch about +X, then roll about -Z (the view direction).
inline Quat FromYawPitchRoll(float yaw, float pitch, float roll) {
    return FromAxisAngle({0.0f, 1.0f, 0.0f}, yaw) * FromAxisAngle({1.0f, 0.0f, 0.0f}, pitch) *
           FromAxisAngle({0.0f, 0.0f, 1.0f}, roll);
}

// Rotation about +Y that points -Z the same way as q's forward vector projected on the floor.
inline Quat YawOnly(const Quat& q) {
    const Vec3 fwd = Rotate(q, {0.0f, 0.0f, -1.0f});
    if (std::abs(fwd.x) < 1e-6f && std::abs(fwd.z) < 1e-6f) {
        return {};
    }
    return FromAxisAngle({0.0f, 1.0f, 0.0f}, std::atan2(-fwd.x, -fwd.z));
}

inline Pose Compose(const Pose& parent, const Pose& child) {
    return {Normalize(parent.orientation * child.orientation),
            parent.position + Rotate(parent.orientation, child.position)};
}

inline Pose Inverse(const Pose& p) {
    const Quat inv = Conjugate(p.orientation);
    return {inv, Rotate(inv, p.position * -1.0f)};
}

} // namespace VR
