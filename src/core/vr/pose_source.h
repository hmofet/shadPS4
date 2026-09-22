// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>
#include "core/vr/vr_types.h"

namespace VR {

// Original PSVR optics, as returned by sceHmdGetFieldOfView with a real headset connected.
// Values are for the left eye: "left" is the outer edge and "right" the inner (nasal) edge.
constexpr FovTangents PsvrFov{
    .left = 1.20743f,
    .right = 1.181346f,
    .up = 1.262872f,
    .down = 1.262872f,
};
constexpr float PsvrIpd = 0.063f;

// Something that can tell where the head and eyes are. Poses are in tracking space
// (see vr_types.h); the PSVR camera offset is applied by the tracker library, not here.
class PoseSource {
public:
    virtual ~PoseSource() = default;

    virtual std::string_view Name() const = 0;

    // Predict the pose at guest process time `guest_time_us`. Returns false if no pose is
    // available (for example before the OpenXR session is running).
    virtual bool Sample(u64 guest_time_us, TrackingSample& out) = 0;

    // Make the current head position and heading the origin.
    virtual void Recenter() = 0;

    // FOV of one eye. Tangents are geometric: "left" is the left edge for both eyes, so the
    // PSVR values (outer edge on the left for the left eye) are mirrored for the right eye.
    virtual FovTangents GetEyeFov(Eye eye) const {
        if (eye == EyeLeft) {
            return PsvrFov;
        }
        return {PsvrFov.right, PsvrFov.left, PsvrFov.up, PsvrFov.down};
    }

    virtual float GetIpd() const {
        return PsvrIpd;
    }
};

// Head pose driven by the keyboard, for working on the libraries without a headset.
// With `use_keyboard` false the head never moves, which is the "static" source.
class DeskPoseSource final : public PoseSource {
public:
    explicit DeskPoseSource(bool use_keyboard);

    std::string_view Name() const override {
        return use_keyboard ? "desk" : "static";
    }
    bool Sample(u64 guest_time_us, TrackingSample& out) override;
    void Recenter() override;

private:
    bool use_keyboard;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
    Vec3 position{};
    u64 last_update_us = 0;
    Quat last_orientation{};
};

} // namespace VR
