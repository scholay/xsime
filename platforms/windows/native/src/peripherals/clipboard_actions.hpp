#pragma once

#include "media.hpp"
#include "../ui/theme.hpp"

namespace rimes::windows::peripherals {
struct PasteTarget {
  HWND foreground = nullptr, focus = nullptr;
  DWORD process = 0, thread = 0;
  bool operator==(const PasteTarget&) const = default;
  explicit operator bool() const { return foreground && focus && process && thread; }
};
PasteTarget CurrentPasteTarget();
enum class PasteResult { Sent, Waiting, TargetChanged, ClipboardChanged, Blocked };
// The fixture replaces these OS boundaries; it never sends input to a real app.
struct PastePlatform {
  std::function<PasteTarget()> target = CurrentPasteTarget;
  std::function<DWORD()> sequence = GetClipboardSequenceNumber;
  std::function<bool()> interactive = InteractiveDesktop;
  std::function<bool()> keys_released;
  std::function<UINT(UINT, INPUT*, int)> send = SendInput;
};
PasteResult PasteClipboardImage(PasteTarget, DWORD sequence, PastePlatform = {});
// Allocate every format before opening/emptying the real clipboard. The fixture
// can replace the publication boundary without touching the user's clipboard.
using ClipboardPublisher = std::function<ClipboardWriteResult(HWND, const std::vector<ClipboardFormat>&, DWORD)>;
ClipboardWriteResult PublishClipboardFormats(HWND, const std::vector<ClipboardFormat>&, DWORD expected_sequence);
ClipboardWriteResult CopyClipboardArchive(HWND, Store&, const Json&, ClipboardPublisher = PublishClipboardFormats,
                          std::function<DWORD()> sequence = GetClipboardSequenceNumber);
bool ClipboardRequiresNativePaste(const Json&);
struct ClipboardActions {
  std::function<ClipboardWriteResult(HWND, const std::string&, bool)> text;
  std::function<ClipboardWriteResult(HWND, Store&, const Json&)> asset;
  std::function<PasteTarget()> target = CurrentPasteTarget;
  std::function<PasteResult(PasteTarget, DWORD)> paste =
      [](PasteTarget target, DWORD sequence) { return PasteClipboardImage(target, sequence); };
  std::function<ULONGLONG()> now = GetTickCount64;
  std::function<ClipboardWriteResult(HWND, Store&, const Json&)> archive =
      [](HWND owner, Store& store, const Json& item) { return CopyClipboardArchive(owner, store, item); };
};
class ClipboardFeedback {
 public:
  ~ClipboardFeedback();
  void Show(const std::wstring&, ui::ThemeId, HWND anchor);
  void Hide();
 private:
  HWND hwnd_ = nullptr;
  static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
};
}  // namespace rimes::windows::peripherals
