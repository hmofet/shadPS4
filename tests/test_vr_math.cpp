// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <numbers>
#include <gtest/gtest.h>
#include "core/vr/vr_types.h"

using namespace VR;

namespace {

constexpr float Eps = 1e-5f;
constexpr float HalfPi = std::numbers::pi_v<float> / 2.0f;
constexpr Vec3 Forward{0.0f, 0.0f, -1.0f};

void ExpectNear(const Vec3& a, const Vec3& b) {
    EXPECT_NEAR(a.x, b.x, Eps);
    EXPECT_NEAR(a.y, b.y, Eps);
    EXPECT_NEAR(a.z, b.z, Eps);
}

// q and -q are the same rotation.
void ExpectSameRotation(const Quat& a, const Quat& b) {
    const float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    EXPECT_NEAR(std::abs(dot), 1.0f, Eps);
}

} // namespace

TEST(VrMath, PositiveYawTurnsLeft) {
    const Quat q = FromYawPitchRoll(HalfPi, 0.0f, 0.0f);
    ExpectNear(Rotate(q, Forward), {-1.0f, 0.0f, 0.0f});
}

TEST(VrMath, PositivePitchLooksUp) {
    const Quat q = FromYawPitchRoll(0.0f, HalfPi, 0.0f);
    ExpectNear(Rotate(q, Forward), {0.0f, 1.0f, 0.0f});
}

TEST(VrMath, YawOnlyDropsPitchAndRoll) {
    const Quat q = FromYawPitchRoll(0.5f, 0.3f, 0.2f);
    ExpectSameRotation(YawOnly(q), FromAxisAngle({0.0f, 1.0f, 0.0f}, 0.5f));
}

TEST(VrMath, YawOnlyOfStraightUpIsIdentity) {
    const Quat q = FromYawPitchRoll(0.0f, HalfPi, 0.0f);
    ExpectSameRotation(YawOnly(q), Quat{});
}

TEST(VrMath, ComposeWithInverseIsIdentity) {
    const Pose p{FromYawPitchRoll(0.7f, -0.2f, 0.1f), {0.3f, 1.6f, -0.4f}};
    const Pose id = Compose(p, Inverse(p));
    ExpectSameRotation(id.orientation, Quat{});
    ExpectNear(id.position, {});
}

TEST(VrMath, ComposeAppliesChildOffsetInParentFrame) {
    // An eye half an IPD to the right of a head turned 90 degrees left ends up in front (-Z).
    const Pose head{FromYawPitchRoll(HalfPi, 0.0f, 0.0f), {0.0f, 1.7f, 0.0f}};
    const Pose eye = Compose(head, {{}, {0.032f, 0.0f, 0.0f}});
    ExpectNear(eye.position, {0.0f, 1.7f, -0.032f});
}
