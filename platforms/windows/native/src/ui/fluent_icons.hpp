#pragma once
#include <Windows.h>
#include <algorithm>
#include <cstdint>

namespace rimes::windows::ui {
// Microsoft Fluent System Icons, unmodified Regular font. Codepoints and
// pinned source/license: resources/fluent-system-icons/{SOURCE.json,LICENSE}.
enum class FluentIcon : wchar_t {
  kSettings = 0xF6A9,
  kHome = 0xF480,
  kKeyboard = 0xF4B8,
  kPalette = 0xF592,
  kBuffer = 0xE156,
  kConnector = 0xE999,
  kPlugin = 0xE9E9,
  kMaintenance = 0xF82E,
  kChord = 0xE74C,
  kCapsule = 0xF139,
  kMailbox = 0xF506,
  kTyping = 0xED9B,
  kStatistics = 0xE2DD,
  kText = 0xF7E4,
  kGrid = 0xF462,
  kLanguage = 0xF833,
  kCheck = 0xF294,
  kInfo = 0xF4A3,
  kRadioOff = 0xF2BB,
  kRadioOn = 0xF644,
  kChevronDown = 0xF2A3,
  kAdd = 0xF109,
  kEdit = 0xF3DD,
  kPanelLeft = 0xE8B0,
  kPanelRight = 0xE8BD,
  kArrowLeft = 0xF15B,
  kChevronLeft = 0xF2AA,
  kClose = 0xF369,
  kMore = 0xE824,
  kCopy = 0xF32B,
  kPaste = 0xF2D5,
  kSend = 0xF699,
  kStop = 0xF72A,
  kFolder = 0xF418,
  kEye = 0xE5F2,
  kCamera = 0xF254,
  kClipboard = 0xE369,
  kNotes = 0xF56B,
  kKey = 0xF4B6,
  kTerminal = 0xEE6F,
  kChat = 0xF286,
  kInbox = 0xF514,
  kSparkle = 0xEB33,
  kSpeakerOn = 0xEB42,
  kSpeakerOff = 0xEB4F,
  kSwap = 0xF18D,
  kSpinner = 0xF709,
  kTimerOff = 0xED8E,
  kArrowDown = 61776, // ic_fluent_arrow_download_20_regular (bundled font map)
  kPin = 62977, // ic_fluent_pin_20_regular
};
inline bool EnsureFluentIcons() {
  struct Resource {
    HANDLE font = nullptr;
    Resource() {
      const auto module = GetModuleHandleW(nullptr);
      const auto resource = FindResourceW(module, MAKEINTRESOURCEW(301), RT_RCDATA);
      const auto loaded = resource ? LoadResource(module, resource) : nullptr;
      DWORD count = 0;
      if (loaded) font = AddFontMemResourceEx(LockResource(loaded),
          SizeofResource(module, resource), nullptr, &count);
    }
    ~Resource() { if (font) RemoveFontMemResourceEx(font); }
  };
  static Resource resource;
  return resource.font != nullptr;
}
inline void DrawFluentIcon(HDC dc, FluentIcon icon, RECT box, COLORREF color) {
  if (!EnsureFluentIcons()) return;
  const auto size = (std::min)(box.right - box.left, box.bottom - box.top);
  const auto font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
      DEFAULT_PITCH, L"FluentSystemIcons-Regular");
  const auto old = SelectObject(dc, font);
  const auto prior_color = SetTextColor(dc, color);
  const auto prior_background = SetBkMode(dc, TRANSPARENT);
  const wchar_t glyph = static_cast<wchar_t>(icon);
  DrawTextW(dc, &glyph, 1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SetBkMode(dc, prior_background);
  SetTextColor(dc, prior_color);
  SelectObject(dc, old);
  DeleteObject(font);
}
inline HICON CreateSettingsIcon(int size) {
  BITMAPINFO info{};
  info.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
  void* pixels = nullptr;
  const auto dc = CreateCompatibleDC(nullptr);
  const auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
  if (!bitmap || !pixels) { if (bitmap) DeleteObject(bitmap); DeleteDC(dc); return nullptr; }
  const auto old = SelectObject(dc, bitmap);
  ZeroMemory(pixels, static_cast<std::size_t>(size) * size * 4);
  DrawFluentIcon(dc, FluentIcon::kSettings, {0,0,size,size}, RGB(255,255,255));
  GdiFlush();
  auto* colors = static_cast<std::uint32_t*>(pixels);
  for (int i = 0; i < size * size; ++i) {
    const auto alpha = colors[i] & 255;
    const auto shade = alpha * 145 / 255;
    colors[i] = (alpha << 24) | (shade << 16) | (shade << 8) | shade;
  }
  SelectObject(dc, old);
  const auto mask = CreateBitmap(size, size, 1, 1, nullptr);
  ICONINFO icon{TRUE, 0, 0, mask, bitmap};
  const auto result = CreateIconIndirect(&icon);
  DeleteObject(mask); DeleteObject(bitmap); DeleteDC(dc);
  return result;
}
}  // namespace rimes::windows::ui
