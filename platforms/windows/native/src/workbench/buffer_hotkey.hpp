#pragma once

#include <Windows.h>
#include <functional>
#include <span>
#include "buffer_hotkey_config.hpp"

namespace rimes::windows::workbench {

enum class HotkeyUpdate { kSaved, kInvalid, kUnavailable, kSaveFailed };

// Owned and used only by the Broker window thread. Keeping the current chord
// registered until the candidate and settings save succeed avoids a restore
// race. No fallback chord is silently substituted after a conflict.
class BufferHotkeyRegistration {
 public:
  explicit BufferHotkeyRegistration(HWND window, int offset = 0) : window_(window), offset_(offset) {}
  ~BufferHotkeyRegistration() { Reset(); }
  BufferHotkeyRegistration(const BufferHotkeyRegistration&) = delete;
  BufferHotkeyRegistration& operator=(const BufferHotkeyRegistration&) = delete;

  bool RegisterInitial(unsigned modifiers, unsigned key) {
    return Update(modifiers, key, [] { return true; }) == HotkeyUpdate::kSaved;
  }
  bool Registered() const noexcept { return id_ != 0; }

  // Follow already-committed runtime settings for a scoped shortcut. An old
  // binding must not survive a failed replacement or a revoked input lease.
  bool Sync(bool enabled, unsigned modifiers, unsigned key) {
    if (!enabled) { Reset(); return true; }
    if (Registered() && (modifiers != modifiers_ || key != key_)) Reset();
    return RegisterInitial(modifiers, key);
  }

  template <typename Save>
  HotkeyUpdate Update(unsigned modifiers, unsigned key, Save save) {
    // A rejected/malformed setting must never reserve an ordinary typing key
    // or a system shortcut. MOD_NOREPEAT is internal, not a user modifier.
    if (!core::ValidShortcut(key, modifiers))
      return HotkeyUpdate::kInvalid;
    if (Registered() && modifiers == modifiers_ && key == key_)
      return save() ? HotkeyUpdate::kSaved : HotkeyUpdate::kSaveFailed;
    const int candidate = id_ == 1 ? 2 : 1;
    // A failed cleanup stays tracked for Reset; never accumulate another
    // registration under an id whose release was not confirmed.
    if (owned_[candidate - 1]) return HotkeyUpdate::kUnavailable;
    if (!RegisterHotKey(window_, offset_ + candidate, modifiers | MOD_NOREPEAT, key))
      return HotkeyUpdate::kUnavailable;
    owned_[candidate - 1] = true;
    bool saved = false;
    try {
      saved = save();
    } catch (...) {
      Release(candidate);
      throw;
    }
    if (!saved) {
      Release(candidate);
      return HotkeyUpdate::kSaveFailed;
    }
    if (Registered()) Release(id_);
    id_ = candidate;
    modifiers_ = modifiers;
    key_ = key;
    return HotkeyUpdate::kSaved;
  }

  bool Matches(WPARAM id, LPARAM chord) const noexcept {
    return Registered() && id == static_cast<WPARAM>(offset_ + id_) &&
           (static_cast<unsigned>(LOWORD(chord)) &
            ~static_cast<unsigned>(MOD_NOREPEAT)) == modifiers_ &&
           HIWORD(chord) == key_;
  }
  void Reset() noexcept {
    Release(1);
    Release(2);
    id_ = 0;
    modifiers_ = key_ = 0;
  }

 private:
  void Release(int id) noexcept {
    if (owned_[id - 1] && UnregisterHotKey(window_, offset_ + id))
      owned_[id - 1] = false;
  }
  HWND window_;
  int offset_ = 0;
  int id_ = 0;
  unsigned modifiers_ = 0, key_ = 0;
  bool owned_[2] = {false, false};
};

struct GlobalHotkeyBinding {
  BufferHotkeyRegistration* registration;
  unsigned modifiers, key;
  bool changed = true;
};

// Keep every old registration until all candidates are reserved and the one
// configuration write succeeds. An inner failure unwinds all reservations.
inline HotkeyUpdate UpdateGlobalHotkeys(std::span<const GlobalHotkeyBinding> bindings,
                                      const std::function<bool()>& save,
                                      std::size_t* failed_index = nullptr) {
  auto failure = HotkeyUpdate::kSaved;
  const auto apply = [&](auto&& next, std::size_t index) -> bool {
    if (index == bindings.size()) {
      if (save()) return true;
      failure = HotkeyUpdate::kSaveFailed; return false;
    }
    const auto& item = bindings[index];
    // An unavailable startup binding remains unavailable until explicitly
    // edited. It must not block an unrelated settings or credential save.
    if (!item.changed) return next(next, index + 1);
    const auto result = item.registration->Update(item.modifiers, item.key, [&] { return next(next, index + 1); });
    if (result == HotkeyUpdate::kInvalid || result == HotkeyUpdate::kUnavailable) {
      failure = result;
      if (failed_index) *failed_index = index;
    }
    return result == HotkeyUpdate::kSaved;
  };
  return apply(apply, 0) ? HotkeyUpdate::kSaved : failure;
}

}  // namespace rimes::windows::workbench
