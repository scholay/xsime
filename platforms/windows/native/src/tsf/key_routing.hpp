#pragma once

#include <Windows.h>
#include "../core/broker_protocol.hpp"

namespace rimes::windows::tsf::detail {

// OnTestKeyDown must not advertise ownership of keys that the product's idle
// engine will decline: dialog hosts can drop such keys before OnKeyDown can
// fail open. No IPC or engine mutation occurs in this prediction.
inline bool RoutePrintableKey(WPARAM key, std::uint32_t modifiers,
                              bool composing, bool capturing,
                              bool ascii_mode, bool ascii_punct,
                              bool caps_lock) noexcept {
  if (composing || capturing) return true;
  if (ascii_mode) return false;
  // Caps Lock is a host-side toggle: the editor already turns the next letter
  // into its upper-case form. Forwarding the key hands the engine an upper-case
  // keycode that Latin schemas cannot spell, and the character is dropped
  // before it reaches the document. Composition state is handled above, so
  // typing Chinese is unaffected once a composition has started.
  if (caps_lock && key >= 'A' && key <= 'Z') return false;
  // Keypad keys retain their numeric/operator identity with Shift. Unlike
  // Shift+number-row punctuation, an idle Rime session can decline them after
  // OnTest has already prevented a native editor from receiving the key.
  if (key >= VK_NUMPAD0 && key <= VK_DIVIDE) return false;
  const bool shifted = (modifiers & static_cast<std::uint32_t>(
      core::KeyModifiers::kShift)) != 0;
  const bool digit = key >= '0' && key <= '9';
  const bool letter = key >= 'A' && key <= 'Z';
  // Shift+number is punctuation, not an idle digit. Preserve the schema's
  // punctuation mapping, candidate numbers, and Buffer's own text capture.
  const bool printable = (shifted || !digit) && key != VK_SPACE;
  // With ascii_punct the engine leaves ASCII punctuation to the host, so this
  // key would be both offered by TSF and declined by Rime. Claiming it strands
  // the character: no component ends up committing it. Latin letters stay
  // claimed because every schema composes them.
  if (ascii_punct && printable && !letter) return false;
  return printable;
}

}  // namespace rimes::windows::tsf::detail
