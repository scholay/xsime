#pragma once
#include <functional>
#include "store.hpp"
namespace rimes::windows::peripherals {
inline constexpr std::size_t kClipboardArchiveLimit = 64 * 1024 * 1024;
inline constexpr std::size_t kClipboardFileLimit = 16;
struct ClipboardFormat {
  // Stable format names, never process-local registered format identifiers.
  std::string name, bytes;
};
struct ClipboardContent {
  bool opened = false;
  std::string text, extension, bytes;
  std::vector<std::filesystem::path> files;
  std::vector<ClipboardFormat> formats;
};
struct ClipboardWriteResult {
  bool success = false;
  // Captured by the writer before releasing OpenClipboard ownership. A later
  // copy must never become the sequence used to authorize this write's paste.
  DWORD sequence = 0;
  explicit operator bool() const { return success; }
};
// Only self-contained HGLOBAL formats are supported. OLE objects, handles,
// delayed file promises and preferred move effects are never replayed.
UINT ClipboardFormatId(const std::string& name);
void ValidateClipboardFormats(const std::vector<ClipboardFormat>&);
std::string ClipboardFileDrop(const std::vector<std::filesystem::path>&);
struct ClipboardReadAccess {
  std::function<bool(const std::string&)> available;
  std::function<std::string(const std::string&, std::size_t)> read;
};
ClipboardContent ReadClipboardSnapshot(const ClipboardReadAccess&);
bool InteractiveDesktop();
ClipboardContent ReadClipboard(HWND owner);
// Keep observation independent of Capsule's window lifetime. A supplied source
// also lets the background collection fixture avoid the user's clipboard.
struct ClipboardSource {
  std::function<DWORD()> sequence = GetClipboardSequenceNumber;
  std::function<bool()> interactive = InteractiveDesktop;
  std::function<ClipboardContent(HWND)> read = ReadClipboard;
};
HBITMAP Thumbnail(const std::filesystem::path& path, unsigned maximum = 1200);
}  // namespace rimes::windows::peripherals
