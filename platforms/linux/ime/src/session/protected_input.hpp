#pragma once

#include <fcitx-utils/capabilityflags.h>

namespace fcitx {
inline bool ProtectedInput(CapabilityFlags flags) {
    // PasswordOrSensitive is a mask; test() would require BOTH bits.
    return flags.testAny(CapabilityFlag::PasswordOrSensitive) ||
           flags.test(CapabilityFlag::Disable);
}
}  // namespace fcitx
