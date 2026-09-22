// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/hmd/hmd_setup_dialog.h"
#include "core/libraries/libs.h"
#include "core/vr/vr_service.h"

namespace Libraries::HmdSetupDialog {

// There is no dialog to show. On real hardware it asks the player to connect the headset; with
// PSVR enabled the headset is already "connected", so the dialog finishes at once with OK.
// Without it, the player is taken to have pressed circle to cancel.

s32 PS4_SYSV_ABI sceHmdSetupDialogInitialize() {
    VR_TRACE(Lib_HmdSetupDialog, "sceHmdSetupDialogInitialize");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogClose() {
    VR_TRACE(Lib_HmdSetupDialog, "sceHmdSetupDialogClose");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogOpen(const OrbisHmdSetupDialogParam* param) {
    VR_TRACE(Lib_HmdSetupDialog, "sceHmdSetupDialogOpen user_id={} handover_disabled={}",
             param ? param->user_id : -1, param ? param->disable_handover_screen : false);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogGetResult(OrbisHmdSetupDialogResult* result) {
    if (result == nullptr) {
        return static_cast<s32>(Libraries::CommonDialog::Error::PARAM_INVALID);
    }
    result->result = VR::IsPsvrEnabled() ? Libraries::CommonDialog::Result::OK
                                         : Libraries::CommonDialog::Result::USER_CANCELED;
    VR_TRACE(Lib_HmdSetupDialog, "sceHmdSetupDialogGetResult -> {}",
             static_cast<s32>(result->result));
    return ORBIS_OK;
}

Libraries::CommonDialog::Status PS4_SYSV_ABI sceHmdSetupDialogUpdateStatus() {
    VR_TRACE(Lib_HmdSetupDialog, "sceHmdSetupDialogUpdateStatus");
    return Libraries::CommonDialog::Status::FINISHED;
}

Libraries::CommonDialog::Status PS4_SYSV_ABI sceHmdSetupDialogGetStatus() {
    VR_TRACE(Lib_HmdSetupDialog, "sceHmdSetupDialogGetStatus");
    return Libraries::CommonDialog::Status::FINISHED;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogTerminate() {
    VR_TRACE(Lib_HmdSetupDialog, "sceHmdSetupDialogTerminate");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("nmHzU4Gh0xs", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogClose);
    LIB_FUNCTION("6lVRHMV5LY0", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogGetResult);
    LIB_FUNCTION("J9eBpW1udl4", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogGetStatus);
    LIB_FUNCTION("NB1Y2kA2jCY", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogInitialize);
    LIB_FUNCTION("NNgiV4T+akU", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogOpen);
    LIB_FUNCTION("+z4OJmFreZc", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogTerminate);
    LIB_FUNCTION("Ud7j3+RDIBg", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogUpdateStatus);
};

} // namespace Libraries::HmdSetupDialog