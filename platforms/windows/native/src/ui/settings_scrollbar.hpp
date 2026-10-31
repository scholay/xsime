// Copyright 2026 scholay. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Windows.h>
#include <algorithm>

#include "icons.hpp"
#include "theme.hpp"

namespace rimes::windows::ui {

// Native scrollbar state/input/accessibility are retained. Its presentation
// belongs to our paint callback: SetScrollInfo(..., TRUE) draws an intermediate
// system scrollbar directly, before that callback can paint the themed frame.
inline bool LayoutSettingsScrollbar(HWND bar, const RECT& bounds, int content,
                                    int viewport, int position) {
  if (!bar) return false;
  content = (std::max)(0, content);
  viewport = (std::max)(1, viewport);
  const bool show = content > viewport;
  const bool visible = (GetWindowLongPtrW(bar, GWL_STYLE) & WS_VISIBLE) != 0;
  RECT previous{};
  GetWindowRect(bar, &previous);
  MapWindowPoints(nullptr, GetParent(bar), reinterpret_cast<POINT*>(&previous), 2);
  SCROLLINFO next{sizeof(next), SIF_RANGE | SIF_PAGE | SIF_POS};
  next.nMax = (std::max)(0, content - 1);
  next.nPage = static_cast<UINT>((std::min)(viewport, (std::max)(1, content)));
  next.nPos = (std::clamp)(position, 0, (std::max)(0, content - viewport));
  SCROLLINFO current{sizeof(current), SIF_RANGE | SIF_PAGE | SIF_POS};
  const bool range_changed = !GetScrollInfo(bar, SB_CTL, &current) ||
      current.nMin != next.nMin || current.nMax != next.nMax ||
      current.nPage != next.nPage || current.nPos != next.nPos;
  if (!show && visible) {
    ShowWindow(bar, SW_HIDE);
    InvalidateRect(GetParent(bar), &previous, FALSE);
  }
  if (range_changed) SetScrollInfo(bar, SB_CTL, &next, FALSE);
  const bool moved = !EqualRect(&previous, &bounds);
  if (show && moved) {
    SetWindowPos(bar, nullptr, bounds.left, bounds.top,
                 bounds.right - bounds.left, bounds.bottom - bounds.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    if (visible) InvalidateRect(GetParent(bar), &previous, FALSE);
  }
  if (show && !visible) ShowWindow(bar, SW_SHOWNA);
  const bool changed = range_changed || (show && moved) || show != visible;
  if (show && changed) InvalidateRect(bar, nullptr, FALSE);
  return changed;
}

inline void PaintSettingsScrollbar(HWND bar, HDC destination,
                                   const ThemePalette& palette, unsigned dpi) {
  if (!bar || !destination) return;
  RECT r{};
  GetClientRect(bar, &r);
  if (r.right <= 0 || r.bottom <= 0) return;
  const auto px = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96); };
  const auto paint = [&](HDC dc) {
    FillRectColor(dc, r, ToColorRef(palette.settings_background));
    SCROLLBARINFO info{sizeof(info)};
    if (!GetScrollBarInfo(bar, OBJID_CLIENT, &info)) return;
    const bool pressed = (info.rgstate[3] & STATE_SYSTEM_PRESSED) != 0;
    POINT cursor{}; GetCursorPos(&cursor); ScreenToClient(bar, &cursor);
    const bool hover = PtInRect(&r, cursor) != FALSE;
    const int width = px(hover || pressed ? 7 : 5);
    RECT thumb{(r.right - width) / 2, info.xyThumbTop,
               (r.right + width) / 2, info.xyThumbBottom};
    if (thumb.bottom > thumb.top)
      FillRoundRect(dc, thumb, px(3), ToColorRef(pressed ? palette.text_secondary :
          Blend(palette.settings_background, palette.text_secondary, hover ? .65f : .45f)));
    for (bool up : {true, false}) {
      RECT arrow{0, up ? 0 : r.bottom - info.dxyLineButton,
                 r.right, up ? info.dxyLineButton : r.bottom};
      const int saved = SaveDC(dc);
      if (up) {
        SetGraphicsMode(dc, GM_ADVANCED);
        XFORM rotate{-1, 0, 0, -1, static_cast<FLOAT>(arrow.right), static_cast<FLOAT>(arrow.bottom)};
        ModifyWorldTransform(dc, &rotate, MWT_LEFTMULTIPLY);
      }
      DrawFluentIcon(dc, FluentIcon::kChevronDown, arrow, ToColorRef(palette.text_muted));
      RestoreDC(dc, saved);
    }
  };
  const auto frame = CreateCompatibleDC(destination);
  const auto bitmap = frame ? CreateCompatibleBitmap(destination, r.right, r.bottom) : nullptr;
  if (!bitmap) {
    if (frame) DeleteDC(frame);
    paint(destination);
    return;
  }
  const auto old = SelectObject(frame, bitmap);
  paint(frame);
  BitBlt(destination, 0, 0, r.right, r.bottom, frame, 0, 0, SRCCOPY);
  SelectObject(frame, old); DeleteObject(bitmap); DeleteDC(frame);
}

}  // namespace rimes::windows::ui
