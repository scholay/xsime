#pragma once

#include <cstdint>
#include <string>

#include "../core/broker_protocol.hpp"

namespace rimes::windows::engine {

// Read the deployed schema in order. Conflicting or unknown bindings for a
// plain symbol may be conditional/custom actions; never bypass them just
// because another entry looks like a familiar paging shortcut. This does not infer
// letter/number bindings or modify the user's config.
template<class ReadString>
std::uint32_t ReadCandidatePageKeys(ReadString&& read) {
  bool key_binder = false;
  for (unsigned i = 0; i < 128; ++i)
    if (read("engine/processors/@" + std::to_string(i)) == "key_binder") key_binder = true;
  if (!key_binder) return 0;
  std::uint32_t minus_flags = 0, equal_flags = 0;
  bool minus_seen = false, equal_seen = false;
  bool minus_blocked = false, equal_blocked = false;
  for (unsigned i = 0; i < 256; ++i) {
    const auto path = "key_binder/bindings/@" + std::to_string(i);
    const auto accept = read(path + "/accept");
    const bool minus = accept == "minus";
    if (!minus && accept != "equal") continue;
    auto& seen = minus ? minus_seen : equal_seen;
    auto& blocked = minus ? minus_blocked : equal_blocked;
    auto& saved = minus ? minus_flags : equal_flags;
    const auto when = read(path + "/when");
    const auto send = read(path + "/send");
    if ((when != "has_menu" && when != "paging") ||
        (send != "Page_Up" && send != "Page_Down")) { blocked = true; continue; }
    const auto direction = minus
        ? (send == "Page_Up" ? core::InputStateFlags::kMinusPageUp : core::InputStateFlags::kMinusPageDown)
        : (send == "Page_Up" ? core::InputStateFlags::kEqualPageUp : core::InputStateFlags::kEqualPageDown);
    auto flags = static_cast<std::uint32_t>(direction);
    if (when == "paging") flags |= static_cast<std::uint32_t>(minus
        ? core::InputStateFlags::kMinusPagingOnly : core::InputStateFlags::kEqualPagingOnly);
    if (seen && saved != flags) blocked = true;
    seen = true; saved = flags;
  }
  return (minus_blocked ? 0U : minus_flags) | (equal_blocked ? 0U : equal_flags);
}

}  // namespace rimes::windows::engine
