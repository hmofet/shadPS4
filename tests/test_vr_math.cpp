// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include <limits>
#include <numbers>
#include <gtest/gtest.h>
#include "core/vr/hmd_frame.h"
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

// ---- sceHmdReprojectionStart parameter decoding (hmd_frame.h) --------------------------------
//
// Values are the ones WipEout Omega Collection passed with the FOV shadPS4 reported for a Quest 3
// (tan out 1.3763819, in 0.8390996, top 0.96568877, bottom 1.4281479), taken from the M0 trace.

namespace {

float F(u32 bits) {
    return std::bit_cast<float>(bits);
}

constexpr float FovEps = 1e-3f;

} // namespace

TEST(HmdFrame, LeftEyeProjectionGivesReportedFov) {
    const StartEyeProjection left{F(0x3ee719db), F(0x3ed5e1f1), F(0x3f1f0aa7), F(0x3ece8b44)};
    FovTangents fov;
    ASSERT_TRUE(ProjectionToFov(left, fov));
    EXPECT_NEAR(fov.left, 1.3763819f, FovEps);  // outer edge
    EXPECT_NEAR(fov.right, 0.8390996f, FovEps); // nasal edge
    EXPECT_NEAR(fov.up, 0.96568877f, FovEps);
    EXPECT_NEAR(fov.down, 1.4281479f, FovEps);
}

TEST(HmdFrame, RightEyeProjectionIsMirrored) {
    const StartEyeProjection right{F(0x3ee719db), F(0x3ed5e1f1), F(0x3ec1eab0), F(0x3ece8b44)};
    FovTangents fov;
    ASSERT_TRUE(ProjectionToFov(right, fov));
    EXPECT_NEAR(fov.left, 0.8390996f, FovEps);
    EXPECT_NEAR(fov.right, 1.3763819f, FovEps);
    EXPECT_NEAR(fov.up, 0.96568877f, FovEps);
    EXPECT_NEAR(fov.down, 1.4281479f, FovEps);
}

TEST(HmdFrame, SymmetricProjectionRoundTrips) {
    // A 90 degree square frustum: tangents of 1 on every side.
    const StartEyeProjection p{0.5f, 0.5f, 0.5f, 0.5f};
    FovTangents fov;
    ASSERT_TRUE(ProjectionToFov(p, fov));
    EXPECT_NEAR(fov.left, 1.0f, Eps);
    EXPECT_NEAR(fov.right, 1.0f, Eps);
    EXPECT_NEAR(fov.up, 1.0f, Eps);
    EXPECT_NEAR(fov.down, 1.0f, Eps);
}

TEST(HmdFrame, RejectsImplausibleProjections) {
    FovTangents fov;
    EXPECT_FALSE(ProjectionToFov({0.0f, 0.5f, 0.5f, 0.5f}, fov)); // zero scale
    EXPECT_FALSE(ProjectionToFov({0.5f, 0.5f, 1.5f, 0.5f}, fov)); // centre off the image
    EXPECT_FALSE(ProjectionToFov({0.5f, 0.5f, 0.5f, 0.0f}, fov)); // centre on the edge
    EXPECT_FALSE(ProjectionToFov({5.0f, 0.5f, 0.5f, 0.5f}, fov)); // 11 degree frustum
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(ProjectionToFov({nan, 0.5f, 0.5f, 0.5f}, fov));
    // Pointer-sized garbage read as floats.
    EXPECT_FALSE(ProjectionToFov({F(0x0c415ef8), F(0x00000000), F(0x0c416278), F(0)}, fov));
}

TEST(HmdFrame, RenderPoseMovesBackToTrackingSpace) {
    // Camera space (-0.133, -0.222, 1.300); the tracker had added the 1.5 m camera distance.
    StartRenderPose p{};
    p.position_x = F(0xbe07e547);
    p.position_y = F(0xbe63073e);
    p.position_z = F(0x3fa6625d);
    p.orientation_x = F(0x3f305540);
    p.orientation_y = F(0xbe1d1733);
    p.orientation_z = F(0xbdd0e975);
    p.orientation_w = F(0xbf337eac);
    p.timestamp = 0x2da855;
    Pose pose;
    u64 timestamp = 0;
    ASSERT_TRUE(DecodeRenderPose(p, 1.5f, pose, timestamp));
    EXPECT_NEAR(pose.position.x, -0.1327f, 1e-3f);
    EXPECT_NEAR(pose.position.y, -0.2217f, 1e-3f);
    EXPECT_NEAR(pose.position.z, 1.2999f - 1.5f, 1e-3f);
    EXPECT_NEAR(pose.orientation.x, 0.6888f, 1e-3f);
    EXPECT_NEAR(pose.orientation.w, -0.7011f, 1e-3f);
    EXPECT_EQ(timestamp, 2992213u);
}

TEST(HmdFrame, RejectsNonUnitOrientation) {
    StartRenderPose p{};
    p.orientation_w = 2.0f;
    Pose pose;
    u64 timestamp = 0;
    EXPECT_FALSE(DecodeRenderPose(p, 1.5f, pose, timestamp));
    p.orientation_w = 1.0f;
    p.position_x = 1000.0f;
    EXPECT_FALSE(DecodeRenderPose(p, 1.5f, pose, timestamp));
    p.position_x = 0.0f;
    EXPECT_TRUE(DecodeRenderPose(p, 1.5f, pose, timestamp));
}

TEST(HmdFrame, SamePoseMatchesUpToRoundingAndSign) {
    const Pose a{Normalize({0.1f, 0.2f, 0.3f, 0.927f}), {1.0f, 2.0f, 3.0f}};
    Pose b = a;
    b.position.x += 1e-6f;
    EXPECT_TRUE(SamePose(a, b));
    // -q is the same rotation.
    b.orientation = {-a.orientation.x, -a.orientation.y, -a.orientation.z, -a.orientation.w};
    EXPECT_TRUE(SamePose(a, b));
    b = a;
    b.position.z += 0.01f;
    EXPECT_FALSE(SamePose(a, b));
    b = a;
    b.orientation = FromAxisAngle({0.0f, 1.0f, 0.0f}, 0.05f) * a.orientation;
    EXPECT_FALSE(SamePose(a, b));
}
