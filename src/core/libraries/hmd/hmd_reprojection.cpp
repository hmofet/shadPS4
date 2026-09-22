// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/hmd/hmd.h"
#include "core/libraries/hmd/hmd_error.h"
#include "core/libraries/kernel/memory.h"
#include "core/libraries/libs.h"
#include "core/libraries/videoout/video_out.h"
#include "core/memory.h"
#include "core/vr/vr_service.h"
#include "video_core/amdgpu/resource.h"

namespace Libraries::Hmd {

// libSceHmdReprojection takes the game's eye images and the pose they were rendered with, applies
// late reprojection and lens distortion, and scans the result out to the headset. Here the host
// OpenXR runtime does both of those jobs, so the library only tracks whether the game is in VR
// mode; the presenter sends the flipped frame to the headset while it is.
//
// The parameter layouts of these calls are not public. Each entry point therefore takes the six
// integer argument registers and logs them, with a dump of the guest memory behind any argument
// that points at readable memory. That trace (M0 in docs/psvr-openxr/DESIGN.md) is what the buffer
// hand-off will be decoded from.

namespace {

bool IsReadable(u64 value, u64 size) {
    // Small values are handles, sizes or flags. The bounds check keeps VirtualQuery (which warns
    // on free memory) away from most non-pointers.
    constexpr u64 MinPointer = 0x100000;
    auto* memory = Core::Memory::Instance();
    Libraries::Kernel::OrbisVirtualQueryInfo info{};
    return value >= MinPointer && (value & 3) == 0 && memory->IsValidMapping(value, size) &&
           memory->VirtualQuery(value, 0, &info) == 0 && info.end >= value + size &&
           (info.protection & static_cast<s32>(Core::MemoryProt::CpuRead)) != 0;
}

// Formats an argument; if it points at readable memory, dumps it, and at the top level also
// dumps whatever the pointers inside it point at, since parameter structs usually hold pointers
// to the buffer descriptions that matter.
std::string DescribeArg(u64 value, bool top_level = true) {
    const u64 dump_bytes = top_level ? 128 : 64;
    if (!IsReadable(value, dump_bytes)) {
        return fmt::format("{:#x}", value);
    }
    const auto* words = reinterpret_cast<const u32*>(value);
    std::string out = fmt::format("{:#x} -> [", value);
    for (u64 i = 0; i < dump_bytes / sizeof(u32); i++) {
        out += fmt::format("{}{:08x}", i == 0 ? "" : " ", words[i]);
    }
    out += "]";
    if (top_level) {
        const auto* qwords = reinterpret_cast<const u64*>(value);
        for (u64 i = 0; i < dump_bytes / sizeof(u64); i++) {
            if (qwords[i] != value && IsReadable(qwords[i], 64)) {
                out += fmt::format(" {{+{:#x}: {}}}", i * 8, DescribeArg(qwords[i], false));
            }
        }
    }
    return out;
}

void TraceCall(const char* name, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    VR_TRACE(Lib_Hmd, "{}({}, {}, {}, {}, {}, {})", name, DescribeArg(a0), DescribeArg(a1),
             DescribeArg(a2), DescribeArg(a3), DescribeArg(a4), DescribeArg(a5));
}

s32 StartVariant(const char* name, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    TraceCall(name, a0, a1, a2, a3, a4, a5);
    if (VR::IsPsvrEnabled()) {
        VR::SetReprojectionActive(true, name);
    }
    return ORBIS_OK;
}

bool ReadTextureSharp(u64 address, AmdGpu::Image& out) {
    if (!IsReadable(address, sizeof(AmdGpu::Image))) {
        return false;
    }
    std::memcpy(&out, reinterpret_cast<const void*>(address), sizeof(AmdGpu::Image));
    return out.Valid();
}

// sceHmdReprojectionStart's first argument starts with pointers to the left and right eye
// texture descriptors (T#), 32 bytes each. Observed in WipEout Omega Collection: both point into
// one 2D array texture (1344x1512 RGBA8 sRGB), base_array 0 for the left eye and 1 for the
// right, alternating between two such textures from frame to frame.
void SubmitEyeTextures(u64 param) {
    if (!IsReadable(param, 2 * sizeof(u64))) {
        return;
    }
    const auto* eye_pointers = reinterpret_cast<const u64*>(param);
    AmdGpu::Image left{};
    AmdGpu::Image right{};
    if (!ReadTextureSharp(eye_pointers[0], left) || !ReadTextureSharp(eye_pointers[1], right)) {
        static bool warned = false;
        if (!warned) {
            LOG_WARNING(Lib_Hmd, "sceHmdReprojectionStart: eye textures not recognised");
            warned = true;
        }
        return;
    }
    VR_TRACE(Lib_Hmd, "  eyes: {:#x} {}x{} layer {} / {:#x} layer {}", left.Address(),
             left.width + 1, left.height + 1, static_cast<u32>(left.base_array), right.Address(),
             static_cast<u32>(right.base_array));
    Libraries::VideoOut::SubmitHmdFrame(left, right);
}

s32 Stop(const char* name, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    TraceCall(name, a0, a1, a2, a3, a4, a5);
    if (VR::IsPsvrEnabled()) {
        VR::SetReprojectionActive(false, name);
    }
    return ORBIS_OK;
}

} // namespace

#define REPROJECTION_TRACE(name) TraceCall(#name, a0, a1, a2, a3, a4, a5)

s32 PS4_SYSV_ABI sceHmdReprojectionStartMultilayer(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    return StartVariant("sceHmdReprojectionStartMultilayer", a0, a1, a2, a3, a4, a5);
}

s32 PS4_SYSV_ABI sceHmdReprojectionAddDisplayBuffer(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
                                                    u64 a5) {
    REPROJECTION_TRACE(sceHmdReprojectionAddDisplayBuffer);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionClearUserEventEnd(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
                                                     u64 a5) {
    REPROJECTION_TRACE(sceHmdReprojectionClearUserEventEnd);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionClearUserEventStart(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
                                                       u64 a5) {
    REPROJECTION_TRACE(sceHmdReprojectionClearUserEventStart);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionDebugGetLastInfo(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
                                                    u64 a5) {
    REPROJECTION_TRACE(sceHmdReprojectionDebugGetLastInfo);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionDebugGetLastInfoMultilayer(u64 a0, u64 a1, u64 a2, u64 a3,
                                                              u64 a4, u64 a5) {
    REPROJECTION_TRACE(sceHmdReprojectionDebugGetLastInfoMultilayer);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionFinalize(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    REPROJECTION_TRACE(sceHmdReprojectionFinalize);
    if (VR::IsPsvrEnabled()) {
        VR::SetReprojectionActive(false, "sceHmdReprojectionFinalize");
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionFinalizeCapture(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    REPROJECTION_TRACE(sceHmdReprojectionFinalizeCapture);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionInitialize(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    REPROJECTION_TRACE(sceHmdReprojectionInitialize);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionInitializeCapture(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
                                                     u64 a5) {
    REPROJECTION_TRACE(sceHmdReprojectionInitializeCapture);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionQueryGarlicBuffAlign() {
    return 0x100;
}

s32 PS4_SYSV_ABI sceHmdReprojectionQueryGarlicBuffSize() {
    return 0x100000;
}

s32 PS4_SYSV_ABI sceHmdReprojectionQueryOnionBuffAlign() {
    return 0x100;
}

s32 PS4_SYSV_ABI sceHmdReprojectionQueryOnionBuffSize() {
    return 0x810;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetCallback(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    REPROJECTION_TRACE(sceHmdReprojectionSetCallback);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetDisplayBuffers(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
                                                     u64 a5) {
    REPROJECTION_TRACE(sceHmdReprojectionSetDisplayBuffers);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetOutputMinColor(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
                                                     u64 a5) {
    REPROJECTION_TRACE(sceHmdReprojectionSetOutputMinColor);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetUserEventEnd(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    REPROJECTION_TRACE(sceHmdReprojectionSetUserEventEnd);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetUserEventStart(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
                                                     u64 a5) {
    REPROJECTION_TRACE(sceHmdReprojectionSetUserEventStart);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStart(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    // Called once per frame with that frame's eye textures.
    const s32 result = StartVariant("sceHmdReprojectionStart", a0, a1, a2, a3, a4, a5);
    if (VR::IsPsvrEnabled()) {
        SubmitEyeTextures(a0);
    }
    return result;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStart2dVr(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    // Cinematic mode: the game shows a flat picture on a virtual screen. The eye source setting
    // "mono" presents it to both eyes; a proper quad layer is a later milestone.
    return StartVariant("sceHmdReprojectionStart2dVr", a0, a1, a2, a3, a4, a5);
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartCapture(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    REPROJECTION_TRACE(sceHmdReprojectionStartCapture);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartLiveCapture(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
                                                    u64 a5) {
    REPROJECTION_TRACE(sceHmdReprojectionStartLiveCapture);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartMultilayer2(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
                                                    u64 a5) {
    return StartVariant("sceHmdReprojectionStartMultilayer2", a0, a1, a2, a3, a4, a5);
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartWideNear(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    return StartVariant("sceHmdReprojectionStartWideNear", a0, a1, a2, a3, a4, a5);
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartWideNearWithOverlay(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
                                                            u64 a5) {
    return StartVariant("sceHmdReprojectionStartWideNearWithOverlay", a0, a1, a2, a3, a4, a5);
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartWithOverlay(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
                                                    u64 a5) {
    return StartVariant("sceHmdReprojectionStartWithOverlay", a0, a1, a2, a3, a4, a5);
}

s32 PS4_SYSV_ABI sceHmdReprojectionStop(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    return Stop("sceHmdReprojectionStop", a0, a1, a2, a3, a4, a5);
}

s32 PS4_SYSV_ABI sceHmdReprojectionStopCapture(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    REPROJECTION_TRACE(sceHmdReprojectionStopCapture);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStopLiveCapture(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    REPROJECTION_TRACE(sceHmdReprojectionStopLiveCapture);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionUnsetCallback(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    REPROJECTION_TRACE(sceHmdReprojectionUnsetCallback);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionUnsetDisplayBuffers(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
                                                       u64 a5) {
    REPROJECTION_TRACE(sceHmdReprojectionUnsetDisplayBuffers);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_A31A0320D80EAD99(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    REPROJECTION_TRACE(Func_A31A0320D80EAD99);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_B9A6FA0735EC7E49(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    REPROJECTION_TRACE(Func_B9A6FA0735EC7E49);
    return ORBIS_OK;
}

#undef REPROJECTION_TRACE

void RegisterReprojection(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("8gH1aLgty5I", "libsceHmdReprojectionMultilayer", 1, "libSceHmd",
                 sceHmdReprojectionStartMultilayer);
    LIB_FUNCTION("NTIbBpSH9ik", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionAddDisplayBuffer);
    LIB_FUNCTION("94+Ggm38KCg", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionClearUserEventEnd);
    LIB_FUNCTION("mdyFbaJj66M", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionClearUserEventStart);
    LIB_FUNCTION("MdV0akauNow", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionDebugGetLastInfo);
    LIB_FUNCTION("ymiwVjPB5+k", "libSceHmd", 1, "libSceHmd",
                 sceHmdReprojectionDebugGetLastInfoMultilayer);
    LIB_FUNCTION("ZrV5YIqD09I", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionFinalize);
    LIB_FUNCTION("utHD2Ab-Ixo", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionFinalizeCapture);
    LIB_FUNCTION("OuygGEWkins", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionInitialize);
    LIB_FUNCTION("BTrQnC6fcAk", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionInitializeCapture);
    LIB_FUNCTION("TkcANcGM0s8", "libSceHmd", 1, "libSceHmd",
                 sceHmdReprojectionQueryGarlicBuffAlign);
    LIB_FUNCTION("z0KtN1vqF2E", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionQueryGarlicBuffSize);
    LIB_FUNCTION("IWybWbR-xvA", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionQueryOnionBuffAlign);
    LIB_FUNCTION("kLUAkN6a1e8", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionQueryOnionBuffSize);
    LIB_FUNCTION("6CRWGc-evO4", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetCallback);
    LIB_FUNCTION("E+dPfjeQLHI", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetDisplayBuffers);
    LIB_FUNCTION("LjdLRysHU6Y", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetOutputMinColor);
    LIB_FUNCTION("knyIhlkpLgE", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetUserEventEnd);
    LIB_FUNCTION("7as0CjXW1B8", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetUserEventStart);
    LIB_FUNCTION("dntZTJ7meIU", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStart);
    LIB_FUNCTION("q3e8+nEguyE", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStart2dVr);
    LIB_FUNCTION("RrvyU1pjb9A", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartCapture);
    LIB_FUNCTION("XZ5QUzb4ae0", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartLiveCapture);
    LIB_FUNCTION("8gH1aLgty5I", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartMultilayer);
    LIB_FUNCTION("gqAG7JYeE7A", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartMultilayer2);
    LIB_FUNCTION("3JyuejcNhC0", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartWideNear);
    LIB_FUNCTION("mKa8scOc4-k", "libSceHmd", 1, "libSceHmd",
                 sceHmdReprojectionStartWideNearWithOverlay);
    LIB_FUNCTION("kcldQ7zLYQQ", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartWithOverlay);
    LIB_FUNCTION("vzMEkwBQciM", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStop);
    LIB_FUNCTION("F7Sndm5teWw", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStopCapture);
    LIB_FUNCTION("PAa6cUL5bR4", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStopLiveCapture);
    LIB_FUNCTION("0wnZViigP9o", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionUnsetCallback);
    LIB_FUNCTION("iGNNpDDjcwo", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionUnsetDisplayBuffers);
    LIB_FUNCTION("oxoDINgOrZk", "libSceHmd", 1, "libSceHmd", Func_A31A0320D80EAD99);
    LIB_FUNCTION("uab6BzXsfkk", "libSceHmd", 1, "libSceHmd", Func_B9A6FA0735EC7E49);
}
} // namespace Libraries::Hmd
