// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>
#include <type_traits>
#include <utility>

#include "common/types.h"
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
#include "core/guest_cpu/guest_callback.h"
#endif

namespace Core {

/// Calls a function pointer that may point into guest code, such as a callback a game registered
/// with an HLE library. On an x86-64 host this is a plain call. With the FEX guest CPU, a guest
/// address runs through the translator; a host (HLE) address is still called directly.
template <typename R, typename... Params, typename... Args>
R GuestCall([[maybe_unused]] std::string_view label, R(PS4_SYSV_ABI* fn)(Params...),
            Args&&... args) {
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    const auto* address = reinterpret_cast<const void*>(fn);
    if (GuestCpu::IsGuestFunctionAddress(address)) {
        const u64 result = GuestCpu::RunGuestFunctionOrAbort(
            address, label, static_cast<Params>(std::forward<Args>(args))...);
        if constexpr (std::is_void_v<R>) {
            return;
        } else if constexpr (std::is_pointer_v<R>) {
            return reinterpret_cast<R>(result);
        } else {
            return static_cast<R>(result);
        }
    }
#endif
    return fn(std::forward<Args>(args)...);
}

} // namespace Core
