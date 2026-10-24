#include "../ui/layered_frame.hpp"
#include "CandidateWindow.h"

#include <algorithm>
#include <cmath>
#include <mutex>

#include "../ui/icons.hpp"
#include "Guids.h"
#include "ModuleState.h"
#include "candidate_layout.hpp"

namespace rimes::windows::tsf {
namespace {

std::mutex g_snapshot_mutex;
CandidateSnapshot g_last_snapshot;

RECT ToRect(const ScreenPoint& origin, long width, long height) noexcept {
  RECT rect{};
  rect.left = origin.x;
  rect.top = origin.y;
  rect.right = origin.x + width;
  rect.bottom = origin.y + height;
  return rect;
}

unsigned int WindowDpi(HWND window) noexcept {
  using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
  const HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32 != nullptr) {
    const auto get_dpi = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(user32, "GetDpiForWindow"));
    if (get_dpi != nullptr && window != nullptr) {
      const UINT dpi = get_dpi(window);
      if (dpi != 0) return dpi;
    }
  }
  const HDC desktop = GetDC(nullptr);
  const int dpi = desktop != nullptr ? GetDeviceCaps(desktop, LOGPIXELSX) : 96;
  if (desktop != nullptr) ReleaseDC(nullptr, desktop);
  return dpi > 0 ? static_cast<unsigned int>(dpi) : 96U;
}

ScreenRect WorkAreaFromCaret(const RECT& caret) noexcept {
  const POINT probe{caret.left, caret.top};
  const HMONITOR monitor = MonitorFromPoint(probe, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) {
    return {info.rcWork.left, info.rcWork.top, info.rcWork.right,
            info.rcWork.bottom};
  }
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  return {work.left, work.top, work.right, work.bottom};
}

HFONT MakeFont(int height_px, int weight, bool mono) {
  return CreateFontW(-height_px, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                     mono ? L"Consolas" : L"Microsoft YaHei UI");
}

int MeasureText(HDC dc, HFONT font, const std::wstring& text) {
  if (text.empty()) return 0;
  const HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
  SIZE size{};
  GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
  SelectObject(dc, previous);
  return size.cx;
}

}  // namespace

CandidateWindow::~CandidateWindow() {
  Hide();
  if (window_ != nullptr) {
    DestroyWindow(window_);
    window_ = nullptr;
  }
}

bool CandidateWindow::GetLastSnapshot(CandidateSnapshot* snapshot) noexcept {
  if (snapshot == nullptr) return false;
  try {
    std::lock_guard lock(g_snapshot_mutex);
    *snapshot = g_last_snapshot;
    return true;
  } catch (...) {
    return false;
  }
}

void CandidateWindow::PublishSnapshot(
    const CandidateSnapshot& snapshot) noexcept {
  try {
    std::lock_guard lock(g_snapshot_mutex);
    g_last_snapshot = snapshot;
  } catch (...) {
  }
}

CandidateSnapshot CandidateWindow::snapshot() const noexcept {
  return snapshot_;
}

std::optional<std::size_t> CandidateWindow::MatrixDigitIndex(std::size_t digit) const noexcept {
  std::size_t offset = 0;
  for (const auto count : snapshot_.matrix_rows) {
    if (snapshot_.highlighted >= offset && snapshot_.highlighted < offset + count) {
      for (std::size_t i = offset; i < offset + count && i < hit_pills_.size(); ++i) {
        if (hit_pills_[i].width() > 0 && digit-- == 0) return i;
      }
      return std::nullopt;
    }
    offset += count;
  }
  return std::nullopt;
}

std::optional<std::size_t> CandidateWindow::CompactDigitIndex(std::size_t digit) const noexcept {
  if (!IsCompact() || !numeric_labels_ || compact_.pages.empty()) return std::nullopt;
  const auto page = compact_.pages[compact_.page_index];
  return digit < page.end - page.begin ? std::optional(page.begin + digit) : std::nullopt;
}

std::optional<std::size_t> CandidateWindow::CompactPageIndex(int delta) const noexcept {
  if (!IsCompact() || compact_.pages.empty()) return std::nullopt;
  if (delta < 0 && compact_.page_index > 0) return compact_.pages[compact_.page_index - 1].begin;
  if (delta > 0 && compact_.page_index + 1 < compact_.pages.size())
    return compact_.pages[compact_.page_index + 1].begin;
  return std::nullopt;
}

std::optional<std::size_t> CandidateWindow::CompactEdgeIndex(int direction) const noexcept {
  if (!IsCompact() || compact_.pages.empty()) return std::nullopt;
  return direction < 0 ? compact_.pages.back().begin : compact_.pages.front().begin;
}

bool CandidateWindow::CanPage(int direction) const noexcept {
  return IsCompact() && (CompactPageIndex(direction).has_value() ||
      (direction < 0 ? snapshot_.has_previous_page : snapshot_.has_next_page));
}

void CandidateWindow::Hide() noexcept {
  snapshot_ = {};
  hover_index_ = -1;
  pressed_index_ = -1;
  if (window_ && GetCapture() == window_) ReleaseCapture();
  hit_pills_.clear();
  compact_ = {};
  numeric_labels_ = false;
  PublishSnapshot(snapshot_);
  if (window_ != nullptr && IsWindowVisible(window_)) {
    NotifyWinEvent(EVENT_OBJECT_IME_HIDE, window_, OBJID_CLIENT, CHILDID_SELF);
    ShowWindow(window_, SW_HIDE);
  }
}

bool CandidateWindow::EnsureWindow() noexcept {
  if (!smooth_shapes_) smooth_shapes_ = ui::AcquireSmoothShapes();
  if (window_ != nullptr) return true;
  HINSTANCE instance = module::Instance();
  if (instance == nullptr) instance = GetModuleHandleW(nullptr);
  if (instance == nullptr) return false;

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = WindowProcedure;
  window_class.hInstance = instance;
  window_class.lpszClassName = kCandidateWindowClass;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground =
      static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
  window_class.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
  RegisterClassExW(&window_class);

  window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
                                WS_EX_NOACTIVATE | WS_EX_NOINHERITLAYOUT |
                                WS_EX_LAYERED,
                            kCandidateWindowClass, L"", WS_POPUP, 0, 0, 0, 0,
                            owner_, nullptr, instance, this);
  return window_ != nullptr;
}

int CandidateWindow::HitIndex(int x_px, int y_px) const noexcept {
  const unsigned dpi = WindowDpi(window_);
  const float x = static_cast<float>(x_px) * 96.0f / static_cast<float>(dpi);
  const float y = static_cast<float>(y_px) * 96.0f / static_cast<float>(dpi);
  if (x < 0 || y < 0 || x >= layout_width_dip_ || y >= layout_height_dip_)
    return -1;
  for (std::size_t i = 0; i < hit_pills_.size(); ++i)
    if (hit_pills_[i].contains(x, y)) return static_cast<int>(i);
  if (compact_.previous.contains(x, y) && CanPage(-1)) return static_cast<int>(hit_pills_.size());
  if (compact_.next.contains(x, y) && CanPage(1)) return static_cast<int>(hit_pills_.size() + 1);
  return -1;
}

void CandidateWindow::Update(const CandidateSnapshot& snapshot) noexcept {
  if (!snapshot.visible || snapshot.items.empty()) {
    Hide();
    return;
  }
  try {
    if (snapshot_.composition != snapshot.composition ||
        snapshot_.items != snapshot.items ||
        snapshot_.matrix_rows != snapshot.matrix_rows ||
        (!snapshot.matrix_rows.empty() && snapshot_.highlighted != snapshot.highlighted) ||
        snapshot_.page_start != snapshot.page_start) {
      pressed_index_ = -1;
      hover_index_ = -1;
      if (window_ && GetCapture() == window_) ReleaseCapture();
    }
    snapshot_ = snapshot;
    PublishSnapshot(snapshot_);
    if (EnsureWindow()) LayoutAndShow(snapshot_);
  } catch (...) {
    Hide();
  }
}

void CandidateWindow::LayoutAndShow(
    const CandidateSnapshot& snapshot) {
  // Settings/DPI/selection changes invalidate an in-flight press as well as
  // changes to the engine items, because its old rectangle may now mean a page.
  pressed_index_ = hover_index_ = -1;
  if (window_ && GetCapture() == window_) ReleaseCapture();
  const unsigned int dpi = WindowDpi(window_);
  const auto metrics = Metrics();
  const ScreenRect work = WorkAreaFromCaret(snapshot.caret_rect);
  const float available_dip =
      static_cast<float>(work.width()) * 96.0f / static_cast<float>(dpi);

  HDC device = GetDC(window_);
  const int cand_px = ui::ScaleDip(metrics.candidate_font_dip, dpi);
  const int label_px = ui::ScaleDip(metrics.label_font_dip, dpi);
  const int ann_px = ui::ScaleDip(metrics.annotation_font_dip, dpi);
  HFONT cand_bold = MakeFont(cand_px, FW_SEMIBOLD, false);
  HFONT label_font = MakeFont(label_px, FW_SEMIBOLD, false);
  HFONT ann_font = MakeFont(ann_px, FW_NORMAL, false);

  std::vector<ui::CandidateStripItem> items;
  items.reserve(snapshot.items.size());
  numeric_labels_ = std::all_of(snapshot.items.begin(), snapshot.items.end(),
      [](const CandidateItem& item) { return item.label.empty() ||
          (item.label.size() == 1 && item.label[0] >= L'1' && item.label[0] <= L'9'); });
  int digit_width_px = 0;
  if (device && numeric_labels_)
    for (wchar_t digit = L'1'; digit <= L'9'; ++digit)
      digit_width_px = (std::max)(digit_width_px, MeasureText(device, label_font, std::wstring(1, digit)));
  for (const auto& item : snapshot.items) {
    ui::CandidateStripItem out;
    out.label = item.label;
    if (out.label.empty()) {
      const wchar_t key =
          CandidateSelectionKey(item.label, items.size());
      if (key != 0) out.label.assign(1, key);
    }
    // Numeric labels restart within each visible subpage. Measure one stable
    // maximum digit width so changing highlight/page cannot move boundaries.
    if (!vertical_ && snapshot.matrix_rows.empty() && numeric_labels_) out.label = L"8";
    out.text = item.text;
    out.comment = item.comment;
    // Always reserve semibold width so selection never shifts layout.
    int width_px = 0;
    if (device) {
      if (!out.label.empty()) {
        width_px += !vertical_ && snapshot.matrix_rows.empty() && numeric_labels_
            ? digit_width_px : MeasureText(device, label_font, out.label);
        width_px += ui::ScaleDip(4, dpi);
      }
      width_px += MeasureText(device, cand_bold, out.text);
      if (!out.comment.empty()) {
        width_px += ui::ScaleDip(6, dpi);
        width_px += MeasureText(device, ann_font, out.comment);
      }
    } else {
      width_px = static_cast<int>((out.label.size() + out.text.size() +
                                   out.comment.size()) *
                                  cand_px * 0.6);
    }
    out.content_width_dip =
        static_cast<int>(std::ceil(static_cast<float>(width_px) * 96.0f /
                                  static_cast<float>(dpi)));
    items.push_back(std::move(out));
  }

  if (device) ReleaseDC(window_, device);
  if (cand_bold) DeleteObject(cand_bold);
  if (label_font) DeleteObject(label_font);
  if (ann_font) DeleteObject(ann_font);

  compact_ = {};
  ui::CandidateStripLayout layout;
  if (!snapshot.matrix_rows.empty()) {
    layout = ui::LayoutCandidateMatrixMeasured(items, snapshot.matrix_rows, metrics,
        (std::min)(available_dip, 640.f), snapshot.highlighted);
  } else if (vertical_) {
    layout = ui::LayoutCandidateStripMeasured(items, metrics, available_dip, true,
        static_cast<float>((std::max)(1L, work.bottom -
            (std::max)(work.top, snapshot.caret_rect.top))) * 96.f / static_cast<float>(dpi));
  } else {
    compact_ = ui::LayoutCandidateCompactMeasured(items, metrics, available_dip,
        9, snapshot.highlighted, snapshot.has_previous_page || snapshot.has_next_page);
    layout = compact_;
  }
  hit_pills_ = layout.pills;
  snapshot_.visible_indices.clear();
  for (std::size_t i = 0; i < hit_pills_.size(); ++i)
    if (hit_pills_[i].width() > 0 && hit_pills_[i].height() > 0)
      snapshot_.visible_indices.push_back(i);
  matrix_labels_.assign(snapshot.items.size(), L"");
  std::size_t offset = 0;
  for (const auto count : snapshot.matrix_rows) {
    std::size_t number = 0;
    for (std::size_t i = offset; i < offset + count && i < hit_pills_.size(); ++i)
      if (hit_pills_[i].width() > 0) matrix_labels_[i] = std::to_wstring(++number);
    offset += count;
  }
  if (!compact_.pages.empty() && numeric_labels_) {
    const auto page = compact_.pages[compact_.page_index];
    for (std::size_t i = page.begin; i < page.end; ++i)
      matrix_labels_[i] = std::to_wstring(i - page.begin + 1);
  }
  layout_width_dip_ = layout.width_dip;
  layout_height_dip_ = layout.height_dip;

  const long width = (std::max)(
      1L, static_cast<long>(ui::ScaleDipF(layout.width_dip, dpi) + 0.5f));
  const long height = (std::max)(
      1L, static_cast<long>(ui::ScaleDipF(layout.height_dip, dpi) + 0.5f));
  const ScreenRect caret{snapshot.caret_rect.left, snapshot.caret_rect.top,
                         snapshot.caret_rect.right, snapshot.caret_rect.bottom};
  const ScreenPoint origin = PlaceCandidateWindow(caret, width, height, work,
                            ui::ScaleDip(metrics.caret_gap_dip, dpi),
                            vertical_ && snapshot.matrix_rows.empty());
  snapshot_.window_rect = ToRect(origin, width, height);
  PublishSnapshot(snapshot_);

  const bool was_visible = IsWindowVisible(window_) != FALSE;
  SetWindowPos(window_, HWND_TOPMOST, origin.x, origin.y, width, height,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  NotifyWinEvent(was_visible ? EVENT_OBJECT_IME_CHANGE : EVENT_OBJECT_IME_SHOW,
                 window_, OBJID_CLIENT, CHILDID_SELF);
  InvalidateRect(window_, nullptr, TRUE);
}

void CandidateWindow::Paint(HDC) const {
  RECT client{};
  GetClientRect(window_, &client);
  const unsigned dpi = WindowDpi(window_);
  const auto& palette = ui::Palette(theme_);
  const auto metrics = Metrics();

  ui::LayeredFrame frame;
  if (!frame.Resize(client.right, client.bottom)) return;
  HDC mem = frame.dc();
  ui::FillRectColor(mem, client, ui::ToColorRef(palette.candidate));

  const float scale = static_cast<float>(dpi) / 96.0f;
  auto to_px = [&](const ui::DipRect& r) {
    RECT out{};
    out.left = static_cast<LONG>(r.left * scale + 0.5f);
    out.top = static_cast<LONG>(r.top * scale + 0.5f);
    out.right = static_cast<LONG>(r.right * scale + 0.5f);
    out.bottom = static_cast<LONG>(r.bottom * scale + 0.5f);
    return out;
  };

  // Strip background covering all rows.
  if (!snapshot_.items.empty()) {
    ui::DipRect strip{0, 0, layout_width_dip_, layout_height_dip_};
    RECT strip_px = to_px(strip);
    ui::FillRoundRect(mem, strip_px, ui::ScaleDip(metrics.strip_radius_dip, dpi),
                      ui::ToColorRef(palette.candidate));
    ui::StrokeRoundRect(mem, strip_px,
                        ui::ScaleDip(metrics.strip_radius_dip, dpi),
                        ui::ToColorRef(palette.border_strong));
  }

  const int cand_px = ui::ScaleDip(metrics.candidate_font_dip, dpi);
  const int label_px = ui::ScaleDip(metrics.label_font_dip, dpi);
  const int ann_px = ui::ScaleDip(metrics.annotation_font_dip, dpi);
  HFONT label_font = MakeFont(label_px, FW_SEMIBOLD, false);
  HFONT cand_font = MakeFont(cand_px, FW_NORMAL, false);
  HFONT cand_bold = MakeFont(cand_px, FW_SEMIBOLD, false);
  HFONT ann_font = MakeFont(ann_px, FW_NORMAL, false);

  for (std::size_t i = 0; i < snapshot_.items.size() && i < hit_pills_.size();
       ++i) {
    if (hit_pills_[i].width() <= 0) continue;
    RECT pill = to_px(hit_pills_[i]);
    const bool selected =
        snapshot_.highlighted != 0xffff &&
        i == static_cast<std::size_t>(snapshot_.highlighted);
    const bool hovered = hover_index_ >= 0 &&
                         i == static_cast<std::size_t>(hover_index_) && !selected;
    if (selected) {
      ui::FillRoundRect(mem, pill, ui::ScaleDip(metrics.pill_radius_dip, dpi),
                        ui::ToColorRef(palette.selection));
    } else if (hovered) {
      ui::FillRoundRect(mem, pill, ui::ScaleDip(metrics.pill_radius_dip, dpi),
                        ui::ToColorRef(palette.surface_tertiary));
      ui::StrokeRoundRect(mem, pill, ui::ScaleDip(metrics.pill_radius_dip, dpi),
                          ui::ToColorRef(palette.border));
    }

    std::wstring label = (!snapshot_.matrix_rows.empty() || (!compact_.pages.empty() && numeric_labels_))
        ? matrix_labels_[i] : snapshot_.items[i].label;
    if (label.empty()) {
      const wchar_t key = CandidateSelectionKey(label, i);
      if (key) label.assign(1, key);
    }
    RECT cursor = pill;
    cursor.left += ui::ScaleDip(metrics.pill_padding_dip, dpi);
    cursor.right -= ui::ScaleDip(metrics.pill_padding_dip, dpi);
    SetBkMode(mem, TRANSPARENT);
    if (!label.empty() && cursor.left < cursor.right) {
      SetTextColor(mem, ui::ToColorRef(selected ? palette.selection_text
                                                : palette.text_secondary));
      HFONT prev = static_cast<HFONT>(SelectObject(mem, label_font));
      DrawTextW(mem, label.c_str(), static_cast<int>(label.size()), &cursor,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
      SIZE size{};
      GetTextExtentPoint32W(mem, label.c_str(), static_cast<int>(label.size()),
                            &size);
      SelectObject(mem, prev);
      cursor.left += size.cx + ui::ScaleDip(4, dpi);
    }
    if (cursor.left < cursor.right) {
      HFONT use = selected ? cand_bold : cand_font;
      HFONT prev = static_cast<HFONT>(SelectObject(mem, use));
      SetTextColor(mem, ui::ToColorRef(selected ? palette.selection_text
                                                : palette.text_primary));
      DrawTextW(mem, snapshot_.items[i].text.c_str(),
                static_cast<int>(snapshot_.items[i].text.size()), &cursor,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
      SIZE size{};
      GetTextExtentPoint32W(mem, snapshot_.items[i].text.c_str(),
                            static_cast<int>(snapshot_.items[i].text.size()),
                            &size);
      SelectObject(mem, prev);
      cursor.left += size.cx;
    }
    if (!snapshot_.items[i].comment.empty() &&
        cursor.left + ui::ScaleDip(6, dpi) < cursor.right) {
      cursor.left += ui::ScaleDip(6, dpi);
      HFONT prev = static_cast<HFONT>(SelectObject(mem, ann_font));
      SetTextColor(mem, ui::ToColorRef(selected ? palette.selection_text
                                                : palette.text_muted));
      DrawTextW(mem, snapshot_.items[i].comment.c_str(),
                static_cast<int>(snapshot_.items[i].comment.size()), &cursor,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                    DT_END_ELLIPSIS);
      SelectObject(mem, prev);
    }
  }

  HFONT separator_font = MakeFont(label_px, FW_NORMAL, false);
  HFONT previous_separator = static_cast<HFONT>(SelectObject(mem, separator_font));
  SetTextColor(mem, ui::ToColorRef(palette.border_strong));
  for (std::size_t i = 1; i < hit_pills_.size(); ++i) {
    const auto& before = hit_pills_[i - 1];
    const auto& after = hit_pills_[i];
    if (before.width() <= 0 || after.width() <= 0 || before.top != after.top) continue;
    RECT separator = to_px({before.right, before.top, after.left, after.bottom});
    DrawTextW(mem, L"|", 1, &separator,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }
  SelectObject(mem, previous_separator);
  DeleteObject(separator_font);

  // Standard text chevrons keep the pager crisp at the host's DPI; these are
  // font glyphs, not hand-drawn icon paths or externally loaded resources.
  if (compact_.previous.width() > 0) {
    HFONT pager_font = MakeFont(ui::ScaleDip(20, dpi), FW_NORMAL, false);
    const auto previous = SelectObject(mem, pager_font);
    for (int direction : {-1, 1}) {
      RECT box = to_px(direction < 0 ? compact_.previous : compact_.next);
      const bool enabled = CanPage(direction);
      const auto index = static_cast<int>(hit_pills_.size()) + (direction > 0 ? 1 : 0);
      if (enabled && hover_index_ == index)
        ui::FillRoundRect(mem, box, ui::ScaleDip(4, dpi), ui::ToColorRef(palette.surface_tertiary));
      SetTextColor(mem, ui::ToColorRef(enabled ? palette.text_secondary : palette.text_muted));
      DrawTextW(mem, direction < 0 ? L"\u2039" : L"\u203a", 1, &box,
          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(mem, previous); DeleteObject(pager_font);
  }

  if (label_font) DeleteObject(label_font);
  if (cand_font) DeleteObject(cand_font);
  if (cand_bold) DeleteObject(cand_bold);
  if (ann_font) DeleteObject(ann_font);

  frame.RoundAlpha(static_cast<float>(ui::ScaleDip(metrics.strip_radius_dip, dpi)));
  frame.Present(window_);
}

LRESULT CALLBACK CandidateWindow::WindowProcedure(HWND window, UINT message,
                                                  WPARAM wparam,
                                                  LPARAM lparam) {
  CandidateWindow* self = nullptr;
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<CandidateWindow*>(create->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    using EnableNonClientDpiScalingFn = BOOL(WINAPI*)(HWND);
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
      const auto enable = reinterpret_cast<EnableNonClientDpiScalingFn>(
          GetProcAddress(user32, "EnableNonClientDpiScaling"));
      if (enable != nullptr) enable(window);
    }
  } else {
    self = reinterpret_cast<CandidateWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
  }

  switch (message) {
    case WM_DESTROY:
      if (self) self->Hide();
      break;
    case WM_NCDESTROY:
      // Windows destroys owned popups with their host window. Do not retain
      // a stale/recycled HWND when the next document starts composing.
      if (self) {
        self->window_ = nullptr;
        self->owner_ = nullptr;
      }
      SetWindowLongPtrW(window, GWLP_USERDATA, 0);
      break;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      const HDC device = BeginPaint(window, &paint);
      if (self != nullptr && device != nullptr) {
        try { self->Paint(device); } catch (...) { self->Hide(); }
      }
      EndPaint(window, &paint);
      return 0;
    }
    case WM_MOUSEMOVE:
      if (self) {
        const int hit = self->HitIndex(static_cast<short>(LOWORD(lparam)),
                                       static_cast<short>(HIWORD(lparam)));
        if (hit != self->hover_index_) {
          self->hover_index_ = hit;
          InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = window;
        TrackMouseEvent(&track);
      }
      return 0;
    case WM_MOUSELEAVE:
      if (self && self->hover_index_ >= 0) {
        self->hover_index_ = -1;
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONDOWN:
      if (self && self->snapshot_.visible) {
        self->pressed_index_ = self->HitIndex(static_cast<short>(LOWORD(lparam)),
                                             static_cast<short>(HIWORD(lparam)));
        if (self->pressed_index_ >= 0) SetCapture(window);
      }
      return 0;
    case WM_LBUTTONUP:
      if (self) {
        const int pressed = self->pressed_index_;
        const int hit = self->HitIndex(static_cast<short>(LOWORD(lparam)),
                                       static_cast<short>(HIWORD(lparam)));
        self->pressed_index_ = -1;
        if (GetCapture() == window) ReleaseCapture();
        if (self->snapshot_.visible && hit >= 0 && hit == pressed) {
          try {
            if (static_cast<std::size_t>(hit) < self->hit_pills_.size()) {
              if (self->select_) self->select_(static_cast<std::size_t>(hit));
            } else if (self->page_) {
              self->page_(static_cast<std::size_t>(hit) == self->hit_pills_.size() ? -1 : 1);
            }
          }
          catch (...) { self->Hide(); }
        }
      }
      return 0;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
      if (self) self->pressed_index_ = -1;
      return 0;
    case WM_DPICHANGED:
      if (self && self->snapshot_.visible) self->LayoutAndShow(self->snapshot_);
      return 0;
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_ERASEBKGND:
      return 1;
    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace rimes::windows::tsf
