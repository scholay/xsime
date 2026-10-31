// Copyright 2026 scholay. SPDX-License-Identifier: Apache-2.0
#include <richedit.h>
#include <commctrl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <iostream>
#include <algorithm>
#include <atomic>
#include <thread>
#include <fstream>

#include "settings_ui.hpp"

using namespace rimes::windows;
namespace {
int failures = 0;
void Check(bool value, const char* message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
void Pump(workbench::SettingsUiHost& host, unsigned milliseconds = 30) {
  const auto until = GetTickCount64() + milliseconds;
  do {
    MSG message{};
    while (GetTickCount64() < until && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (!host.HandleDialogMessage(&message)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
    }
    Sleep(1);
  } while (GetTickCount64() < until);
}
struct PaintProbe {
  unsigned paints = 0;
  static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM w, LPARAM l,
                              UINT_PTR id, DWORD_PTR data) {
    auto* probe = reinterpret_cast<PaintProbe*>(data);
    if (message == WM_PAINT) ++probe->paints;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, Proc, id);
    return DefSubclassProc(window, message, w, l);
  }
};
void CheckIdleRefresh(workbench::SettingsUiHost& host, HWND panel) {
  SendMessageW(panel, WM_TIMER, 1, 0);
  Pump(host, 250);
  PaintProbe probe;
  SetWindowSubclass(panel, PaintProbe::Proc, 90, reinterpret_cast<DWORD_PTR>(&probe));
  // Drain a deliberate repaint, proving that this observes the production
  // window's paint path before measuring idle timer activity.
  RedrawWindow(panel, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
  Pump(host, 250);  // Drain the composited parent's deferred child frame too.
  const auto forced = probe.paints;
  probe.paints = 0;
  for (int i = 0; i < 6; ++i) {
    for (const int id : {7003, 7004})
      SendMessageW(panel, WM_COMMAND, MAKEWPARAM(id, EN_UPDATE),
                   reinterpret_cast<LPARAM>(GetDlgItem(panel, id)));
    SendMessageW(panel, WM_TIMER, 1, 0);
    UpdateWindow(panel);
  }
  const auto commanded = probe.paints;
  Pump(host, 650);
  std::cout << "Practice idle paint count: " << probe.paints
            << " (notification/timer paints: " << commanded << ", forced paint observed: " << forced << ")\n";
  // The native parent may present one deferred child frame. A recurring
  // invalidation loop produced dozens here; it must never resume at rest.
  Check(commanded == 0 && probe.paints <= 1,
        "idle notifications do not invalidate, and no recurring repaint loop remains");
  RemoveWindowSubclass(panel, PaintProbe::Proc, 90);
}
struct Canvas {
  HDC dc = CreateCompatibleDC(nullptr);
  HBITMAP bitmap = nullptr;
  HGDIOBJ old = nullptr;
  int width, height;
  Canvas(int w, int h) : width(w), height(h) {
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    old = SelectObject(dc, bitmap);
  }
  ~Canvas() {
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
  }
  void Save(const std::filesystem::path& path) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> source;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    auto require = [](HRESULT value) {
      if (FAILED(value)) throw std::runtime_error("PNG export failed");
    };
    require(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                             CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    require(factory->CreateBitmapFromHBITMAP(bitmap, nullptr,
                                             WICBitmapIgnoreAlpha, &source));
    require(factory->CreateStream(&stream));
    require(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    require(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    require(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    require(encoder->CreateNewFrame(&frame, nullptr));
    require(frame->Initialize(nullptr));
    require(frame->SetSize(width, height));
    auto format = GUID_WICPixelFormat24bppBGR;
    require(frame->SetPixelFormat(&format));
    require(frame->WriteSource(source.Get(), nullptr));
    require(frame->Commit());
    require(encoder->Commit());
  }
};
void PaintChildren(HWND window, HDC dc, HWND root) {
  std::vector<HWND> children;
  for (auto c = GetWindow(window, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
    children.push_back(c);
  for (auto it = children.rbegin(); it != children.rend(); ++it) {
    auto child = *it;
    if (!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) continue;
    RECT r{}, client{};
    GetWindowRect(child, &r);
    MapWindowPoints(nullptr, root, reinterpret_cast<POINT*>(&r), 2);
    GetClientRect(child, &client);
    if (client.right <= 0 || client.bottom <= 0) continue;
    Canvas layer(client.right, client.bottom);
    const auto saved = SaveDC(layer.dc);
    SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(layer.dc),
                 PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
    RestoreDC(layer.dc, saved);
    PaintChildren(child, layer.dc, child);
    const auto destination = SaveDC(dc);
    const auto region = CreateRectRgn(0,0,0,0);
    if (GetWindowRgn(child,region) != ERROR) {
      OffsetRgn(region,r.left,r.top); ExtSelectClipRgn(dc,region,RGN_AND);
    }
    BitBlt(dc, r.left, r.top, client.right, client.bottom, layer.dc, 0, 0, SRCCOPY);
    DeleteObject(region); RestoreDC(dc,destination);
  }
}
void Export(HWND hwnd, const std::filesystem::path& path) {
  RECT r{};
  GetClientRect(hwnd, &r);
  Canvas canvas(r.right, r.bottom);
  SendMessageW(hwnd, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(canvas.dc),
               PRF_CLIENT);
  PaintChildren(hwnd, canvas.dc, hwnd);
  canvas.Save(path);
}
std::vector<workbench::PluginView> Plugins() {
  std::vector<workbench::PluginView> items;
  for (auto [id, name, kind] :
       std::vector<std::tuple<std::string, std::string, std::string>>{
           {"builtin.fly-chord-learning", "并击", "extension"},
           {"builtin.typing-speed", "打字测速", "extension"},
           {"builtin.statistics", "统计", "extension"},
           {"builtin.apple-translation", "实时翻译", "buffer"},
           {"builtin.codex-cli", "Codex CLI", "buffer"},
           {"builtin.openai-compatible", "AI 生成", "buffer"},
           {"builtin.capsule.temporary", "临时", "capsule"},
           {"builtin.capsule.capture", "收录", "capsule"},
           {"builtin.capsule.notes", "笔记", "capsule"},
           {"builtin.capsule.resources", "资源", "capsule"},
           {"builtin.capsule.passwords", "密码", "capsule"},
           {"builtin.mailbox.terminal", "终端", "mailbox"},
           {"builtin.mailbox.chat", "对话", "mailbox"},
           {"builtin.mailbox.inbox", "收件", "mailbox"}}) {
    workbench::PluginView p;
    p.id = id;
    p.name = name;
    p.kind = kind;
    p.version = "2.0.1";
    p.installed = p.enabled = p.bundled = true;
    p.can_install = kind == "buffer" || kind == "extension";
    items.push_back(p);
  }
  return items;
}
struct PageProbe {
  unsigned paints = 0, writes = 0, hides = 0, fonts = 0, combo_resets = 0,
           formats = 0;
  std::vector<HWND> windows;
  static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l,
                              UINT_PTR id, DWORD_PTR data) {
    auto& p = *reinterpret_cast<PageProbe*>(data);
    if (m == WM_PAINT && IsWindowVisible(h)) ++p.paints;
    if (m == WM_SETTEXT) ++p.writes;
    if (m == WM_SETFONT) ++p.fonts;
    if (m == CB_RESETCONTENT) ++p.combo_resets;
    if (m == EM_SETCHARFORMAT || m == EM_SETPARAFORMAT) ++p.formats;
    if (m == WM_SHOWWINDOW && !w) ++p.hides;
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h, Proc, id);
    return DefSubclassProc(h, m, w, l);
  }
  void Add(HWND h) {
    windows.push_back(h);
    SetWindowSubclass(h, Proc, 91, reinterpret_cast<DWORD_PTR>(this));
  }
  explicit PageProbe(HWND root) {
    Add(root);
    EnumChildWindows(root, [](HWND h, LPARAM p) -> BOOL {
      reinterpret_cast<PageProbe*>(p)->Add(h); return TRUE;
    }, reinterpret_cast<LPARAM>(this));
  }
  ~PageProbe() { for (auto h : windows) if (IsWindow(h)) RemoveWindowSubclass(h, Proc, 91); }
};
RECT InParent(HWND child, HWND parent) {
  RECT rect{};
  GetWindowRect(child, &rect);
  MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
  return rect;
}
std::wstring EditorText(HWND editor) {
  std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(editor)) + 1, L'\0');
  text.resize(static_cast<std::size_t>(GetWindowTextW(editor, text.data(), static_cast<int>(text.size()))));
  return text;
}
void PracticeButton(HWND panel, int id) {
  SendMessageW(panel, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED),
               reinterpret_cast<LPARAM>(GetDlgItem(panel, id)));
}
void CheckPracticeLayout(workbench::SettingsUiHost& host,
                         const std::shared_ptr<metrics::Service>& service,
                         HWND panel, HWND editor) {
  const auto materials = GetDlgItem(panel, 7014);
  const auto collapse = GetDlgItem(panel, 7006);
  const auto grid = GetDlgItem(panel, 7015);
  const auto readings = GetDlgItem(panel, 7016);
  Check(materials && collapse && grid && readings,
        "practice has native material, grid and live-reading controls");
  Check(!(GetWindowLongPtrW(editor, GWL_STYLE) & WS_VSCROLL),
        "the double track has no independent vertical scrollbar");
  const auto source = GetDlgItem(panel, 7003);
  Check(!source || !IsWindowVisible(source),
        "the old separate source box is replaced by the single double track");
  RECT viewport{};
  GetClientRect(panel, &viewport);
  const auto full = InParent(editor, panel);
  const auto fixed = InParent(materials, panel);
  const auto collapse_rect = InParent(collapse, panel);
  Check(full.bottom - full.top > viewport.bottom && full.right <= fixed.left,
        "the entire article grows vertically beside the material sidebar");
  Check(collapse_rect.left >= fixed.left && collapse_rect.right <= viewport.right,
        "the expanded material sidebar owns its collapse action");
  for (const int id : {7009, 7012, 7007})
    Check(IsWindowVisible(GetDlgItem(panel, id)),
          "the sidebar exposes article and book tabs and EPUB import");
  SendMessageW(panel, WM_VSCROLL, SB_BOTTOM,
               reinterpret_cast<LPARAM>(GetDlgItem(panel, 7200)));
  Pump(host);
  const auto bottom = InParent(editor, panel);
  const auto still_fixed = InParent(materials, panel);
  Check(bottom.top < full.top && bottom.bottom <= viewport.bottom,
        "page scrolling reaches the final double-track row");
  Check(EqualRect(&fixed, &still_fixed) != FALSE,
        "page scrolling leaves the material sidebar fixed in the viewport");
  SendMessageW(panel, WM_VSCROLL, SB_TOP, 0);
  Pump(host);
  PracticeButton(panel, 7006);
  Pump(host);
  const auto expanded_text = InParent(editor, panel);
  Check(!IsWindowVisible(materials) &&
            expanded_text.right > full.right &&
            service->PracticePreferences().value("materialsCollapsed", false),
        "collapsing materials frees its width and immediately saves the preference");
  const auto reopen = InParent(collapse, panel);
  Check(IsWindowVisible(collapse) && reopen.top < expanded_text.top,
        "the page header offers the way to restore hidden materials");
  PracticeButton(panel, 7006);
  PracticeButton(panel, 7015);
  PracticeButton(panel, 7016);
  Pump(host);
  const auto compact = InParent(editor, panel);
  const auto prefs = service->PracticePreferences();
  Check(prefs.value("gridHidden", false) && prefs.value("cellsHidden", false) &&
            !prefs.value("materialsCollapsed", true),
        "grid and live readings are independently persisted from the sidebar");
  Check(compact.top < full.top,
        "hiding the live readings returns their height to the article");
  PracticeButton(panel, 7015);
  PracticeButton(panel, 7016);
  Pump(host);
  Check(!service->PracticePreferences().value("gridHidden", true) &&
            !service->PracticePreferences().value("cellsHidden", true),
        "grid and live readings can be restored immediately");
}
void CheckLivePracticeToggles(workbench::SettingsUiHost& host,
                              const std::shared_ptr<metrics::Service>& service,
                              HWND panel, HWND editor) {
  const auto text = metrics::Characters(workbench::Utf8(EditorText(editor)));
  const auto metrics_before = service->Practice()["metrics"];
  CHARRANGE selection_before{};
  SendMessageW(editor, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection_before));
  for (const int id : {7015, 7016, 7006, 7006, 7016, 7015}) {
    PracticeButton(panel, id);
    Pump(host);
    Check(service->Armed() && GetFocus() == editor,
          "practice display toggles retain the active editor and its focus");
    Check(metrics::Characters(workbench::Utf8(EditorText(editor))) == text &&
              service->Practice()["metrics"]["committed"] == metrics_before["committed"] &&
              service->Practice()["metrics"]["attempts"] == metrics_before["attempts"],
          "practice display toggles preserve text and do not invent scored attempts");
  }
  CHARRANGE selection_after{};
  SendMessageW(editor, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection_after));
  Check(selection_before.cpMin == selection_after.cpMin &&
            selection_before.cpMax == selection_after.cpMax,
        "restoring the same track width preserves editor selection");
}
void CheckCompositionDisplayGuards(workbench::SettingsUiHost& host,
                                   const std::shared_ptr<metrics::Service>& service,
                                   HWND panel, HWND editor) {
  // This exercises the IME message boundary without claiming to drive a real
  // candidate window. The text between START/END stands in for uncommitted code.
  const auto committed_text = EditorText(editor);
  const auto scored = service->Practice()["metrics"];
  SendMessageW(editor, WM_IME_STARTCOMPOSITION, 0, 0);
  const auto marked_text = committed_text + L"ni";
  SetWindowTextW(editor, marked_text.c_str());
  SendMessageW(panel, WM_COMMAND, MAKEWPARAM(7004, EN_CHANGE),
               reinterpret_cast<LPARAM>(editor));
  SendMessageW(panel, WM_APP + 72, 0, 0);
  Pump(host);
  Check(service->Practice()["metrics"]["committed"] == scored["committed"] &&
            service->Practice()["metrics"]["attempts"] == scored["attempts"],
        "uncommitted composition never enters scoring");
  PracticeButton(panel, 7002);
  Pump(host);
  Check(service->Armed() && GetFocus() == editor,
        "finishing while composing asks to settle composition and keeps the editor active");
  {
    PageProbe probe(editor);
    for (const int id : {7015, 7016, 7016, 7015}) {
      PracticeButton(panel, id);
      Pump(host);
    }
    Check(probe.writes == 0 && probe.formats == 0 &&
              EditorText(editor) == marked_text,
          "display toggles do not replace or reformat live IME composition");
  }
  SetWindowTextW(editor, committed_text.c_str());
  SendMessageW(editor, WM_IME_ENDCOMPOSITION, 0, 0);
  Pump(host);
  Check(service->Practice()["metrics"]["committed"] == scored["committed"] &&
            service->Practice()["metrics"]["attempts"] == scored["attempts"],
        "cancelled composition does not create a committed attempt");
}
int CheckHistoryRefresh(const std::filesystem::path& output) {
  std::filesystem::create_directories(output);
  auto service = std::make_shared<metrics::Service>(output / L"history-data");
  workbench::SettingsUiCallbacks cb;
  cb.metrics = service;
  cb.plugins = Plugins;
  cb.load = [] { return workbench::Settings{}; };
  workbench::SettingsUiHost host(cb);
  host.Open(nullptr, false);
  ShowWindow(host.hwnd(), SW_SHOWNOACTIVATE);
  SetWindowPos(host.hwnd(), nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  for (const auto page : {ui::SettingsPage::kTyping, ui::SettingsPage::kStatistics}) {
    service->Arm(host.hwnd(), metrics::Articles().front(),
                 {{"comparison", "history-refresh"}, {"schema", "Regression fixture"}});
    service->Reconcile("天");
    service->Finish("interrupted");
    service->ResetPractice();
    Check(!service->Data()["results"].empty(), "history fixture contains a saved result");
    host.OpenPage(nullptr, page, page == ui::SettingsPage::kTyping ? 1 : 2, false);
    Pump(host, 100);
    const auto panel = FindWindowExW(host.hwnd(), nullptr, L"Rimes.ExtensionPages", nullptr);
    const auto comparison = GetDlgItem(panel, 7010);
    Check(comparison && IsWindowVisible(comparison), "history has a native comparison control");
    PageProbe probe(panel);
    const auto previous = service->Data();
    ValidateRect(panel, nullptr);
    service->Daily('A', false, 0, 2, 1, metrics::Wall());
    Check(service->Data()["days"] != previous["days"], "daily statistics actually changed");
    for (int tick = 0; tick < 3; ++tick) SendMessageW(panel, WM_TIMER, 1, 0);
    Check(probe.combo_resets == 0 && probe.fonts == 0 &&
              !GetUpdateRect(panel, nullptr, FALSE),
          "unrelated daily statistics leave history controls and paint state untouched");
    Check(service->Clear("results"), "history results can still be removed");
    SendMessageW(panel, WM_TIMER, 1, 0);
    Check(probe.combo_resets > 0 && GetUpdateRect(panel, nullptr, FALSE),
          "changed results still rebuild and repaint history controls");
    wchar_t label[64]{};
    SendMessageW(comparison, CB_GETLBTEXT, 0, reinterpret_cast<LPARAM>(label));
    Check(std::wstring(label) == L"暂无输入方案记录", "empty history replaces the old comparison");
  }
  host.Close();
  return failures ? 1 : 0;
}
int AuditPages(const std::filesystem::path& output) {
  std::filesystem::create_directories(output);
  auto service = std::make_shared<metrics::Service>(output / L"audit-data");
  auto plugins = Plugins();
  workbench::Settings config;
  workbench::SettingsUiCallbacks cb;
  cb.metrics = service;
  cb.plugins = [&] { return plugins; };
  cb.load = [&] { return config; };
  cb.save_connections = [&](auto value, const auto&, std::string*) { config = value; return true; };
  cb.connectors.has_secret = [](const auto&) { return false; };
  cb.connectors.probe = [](const auto&, const auto&) {
    return workbench::ConnectionProbeResult{false, "Fixture: disconnected", {}};
  };
  workbench::SettingsUiHost host(cb);
  host.Open(nullptr, false);
  ShowWindow(host.hwnd(), SW_SHOWNOACTIVATE);
  SetWindowPos(host.hwnd(), nullptr, 0, 0, 0, 0,
      SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  auto panel = FindWindowExW(host.hwnd(), nullptr, L"Rimes.ExtensionPages", nullptr);
  nlohmann::json audit = nlohmann::json::array();
  for (auto page : ui::kSettingsNavOrder) {
    for (int sub = 0; sub < ui::SettingsTabCount(page); ++sub) {
      host.OpenPage(nullptr, page, sub, false);
      Pump(host, 350);
      PageProbe probe(host.hwnd());
      RedrawWindow(host.hwnd(), nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
      Pump(host, 150);
      const auto forced = probe.paints;
      probe.paints = probe.writes = probe.hides = 0;
      for (int tick = 0; tick < 6; ++tick) {
        SendMessageW(host.hwnd(), WM_TIMER, 11, 0);
        SendMessageW(panel, WM_TIMER, 1, 0);
        UpdateWindow(host.hwnd());
        UpdateWindow(panel);
      }
      const auto commanded = probe.paints;
      Pump(host, 450);
      std::cout << "Page " << static_cast<int>(page) << '/' << sub
                << ": idle=" << probe.paints << " timer=" << commanded
                << " text=" << probe.writes << " hides=" << probe.hides
                << " forced=" << forced << std::endl;
      audit.push_back({{"page", static_cast<int>(page)}, {"subpage", sub},
          {"idlePaints", probe.paints}, {"timerPaints", commanded},
          {"textWrites", probe.writes}, {"hides", probe.hides}, {"forcedPaints", forced}});
      Check(forced > 0, "audit observes the real visible paint path");
      Check(commanded == 0 && probe.paints <= 1 && probe.writes == 0 && probe.hides == 0,
            "unchanged settings page stays idle across timer ticks");
      // Async status completions, resizing and dropdown changes share this
      // layout path. Retained controls must not disappear between frames.
      probe.hides = 0;
      host.OpenPage(nullptr, page, sub, false);
      Check(probe.hides == 0, "same-page layout does not hide retained controls");
      audit.back()["relayoutHides"] = probe.hides;
    }
  }
  host.OpenPage(nullptr, ui::SettingsPage::kAppearance, 1, false);
  Pump(host, 200);
  const auto edit = GetDlgItem(host.hwnd(), 6033);
  SetFocus(edit);
  SendMessageW(edit, EM_SETSEL, 1, 2);
  {
    PageProbe probe(host.hwnd());
    SetWindowTextW(edit, L"720");
    SendMessageW(edit, EM_SETSEL, 1, 2);
    SendMessageW(host.hwnd(), WM_TIMER, 12, 0);
    DWORD first = 0, last = 0;
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
    Check(config.candidate_width == 720, "autosave remains immediate");
    Check(probe.fonts == 0 && probe.hides == 0, "autosave does not reset fonts or hide editors");
    Check(GetFocus() == edit && first == 1 && last == 2, "autosave preserves editor focus and selection");
    host.OpenPage(nullptr, ui::SettingsPage::kAppearance, 1, false);
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
    Check(GetFocus() == edit && first == 1 && last == 2 && probe.hides == 0,
          "same-page layout preserves editor focus and selection");
  }
  host.OpenPage(nullptr, ui::SettingsPage::kStatistics, 0, false);
  Pump(host, 200);
  ValidateRect(panel, nullptr);
  service->Daily('A', false, 0, 2, 1, metrics::Wall());
  SendMessageW(panel, WM_TIMER, 1, 0);
  Check(GetUpdateRect(panel, nullptr, FALSE), "new statistics still invalidate the page");
  host.OpenPage(nullptr, ui::SettingsPage::kPlugins, 4, false);
  Pump(host, 200);
  ValidateRect(panel, nullptr);
  plugins[0].name += " updated";
  SendMessageW(panel, WM_TIMER, 1, 0);
  Check(GetUpdateRect(panel, nullptr, FALSE), "changed plugin metadata refreshes the gallery");
  Pump(host, 200);
  ValidateRect(panel, nullptr);
  SendMessageW(panel, WM_TIMER, 1, 0);
  Check(!GetUpdateRect(panel, nullptr, FALSE), "gallery returns to idle after the change");
  std::ofstream(output / L"page-repaint-audit.json") << audit.dump(2) << '\n';
  host.Close();
  return failures ? 1 : 0;
}
int Run(const std::filesystem::path& output, bool typing_only = false) {
  std::filesystem::create_directories(output);
  const auto storage = output / L"fixture-data";
  auto service = std::make_shared<metrics::Service>(storage);
  Check(service->SetPracticePreferences({{"materialsCollapsed", false},
                                         {"gridHidden", false},
                                         {"cellsHidden", false},
                                         {"lastArticleID", metrics::Articles().front().id}}),
        "practice fixture starts with deterministic display preferences");
  auto plugins = Plugins();
  workbench::Settings config;
  ui::ThemeId theme = ui::ThemeId::kNight;
  for (int day = 30; day >= 0; --day) {
    double start = metrics::Wall() - day * 86400;
    for (int n = 0; n < 45; ++n)
      service->Daily('A' + n % 26, false, 0, n % 3 == 0 ? 2 : 0,
                     n % 5 == 0 ? 1 : 0, start + n * .8);
  }
  workbench::MigrateConnections(&config);
  config.connections.front().documentation_url = "https://example.com/docs";
  std::atomic<bool> authorized{false}, finish_login{false};
  workbench::SettingsUiCallbacks cb;
  cb.connectors.has_secret = [](const auto&) { return false; };
  cb.connectors.probe = [&](const auto&, const auto&) { return workbench::ConnectionProbeResult{authorized.load(), authorized ? "ChatGPT 已就绪。" : "尚未登录，请完成浏览器授权。", {}}; };
  cb.connectors.login = [&](const auto&, const auto& cancelled, const auto& url, std::string*) {
    url("https://auth.openai.com/oauth/authorize?state=fixture&code_challenge=fixture");
    const auto start = GetTickCount64();
    while (!finish_login && !cancelled() && GetTickCount64()-start < 3000) Sleep(2);
    authorized = finish_login && !cancelled(); return authorized.load();
  };
  cb.connectors.copy_private = [](const auto&) { return true; };
  cb.save_connections = [&](auto value, const auto&, std::string*) { config = value; return true; };
  cb.metrics = service;
  cb.load = [&] { return config; };
  cb.load_theme = [&] { return theme; };
  cb.plugins = [&] { return plugins; };
  cb.manage_plugin = [&](const auto& id, const auto& action, std::string*) {
    for (auto& p : plugins)
      if (p.id == id) {
        p.enabled = action == "enable";
        return true;
      }
    return false;
  };
  workbench::SettingsUiHost host(cb);
  host.OpenPage(nullptr, ui::SettingsPage::kTyping, 0, true);
  Pump(host);
  auto panel =
      FindWindowExW(host.hwnd(), nullptr, L"Rimes.ExtensionPages", nullptr);
  auto editor = GetDlgItem(panel, 7004);
  Check(panel && editor, "real native practice controls");
  CheckIdleRefresh(host, panel);
  CheckPracticeLayout(host, service, panel, editor);
  SendMessageW(panel, WM_COMMAND, MAKEWPARAM(7001, BN_CLICKED),
               reinterpret_cast<LPARAM>(GetDlgItem(panel, 7001)));
  Pump(host);
  Check(service->Armed() && GetFocus() == editor,
        "explicit start focuses own editor");
  auto daily = service->Data()["days"];
  core::KeyEvent key{};
  key.virtual_key = 'A';
  key.timestamp_millis = 44;
  key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
  service->ObserveKey(GetCurrentProcessId(), key, false);
  service->LocalKey('A', 0, false, 44, false);
  service->ObserveChord(GetCurrentProcessId());
  service->ObserveCommit(GetCurrentProcessId(), 1);
  Check(service->Practice()["metrics"]["keys"] == 1,
        "authoritative and local keys deduplicated");
  Check(service->Data()["days"] == daily,
        "practice excluded from daily observations");
  MSG enter{};
  enter.hwnd = editor;
  enter.message = WM_KEYDOWN;
  enter.wParam = VK_RETURN;
  Check(!host.HandleDialogMessage(&enter),
        "Return remains editor input, not Save");
  SendMessageW(editor, WM_CHAR, L'天', 0);
  Pump(host);
  Check(service->Practice()["metrics"]["committed"] == 1,
        "native editor committed change scored");
  // Active timing must keep updating, but its dirty region must exclude the
  // article and editable text rather than clearing the entire panel.
  SendMessageW(panel, WM_VSCROLL, SB_TOP, 0);
  UpdateWindow(panel);
  ValidateRect(panel, nullptr);
  SendMessageW(panel, WM_TIMER, 1, 0);
  HRGN dirty = CreateRectRgn(0, 0, 0, 0);
  const auto region = GetUpdateRgn(panel, dirty, FALSE);
  if (region != NULLREGION && region != ERROR) {
    const auto input = InParent(editor, panel);
    const auto readings = InParent(GetDlgItem(panel, 7016), panel);
    const auto materials = InParent(GetDlgItem(panel, 7014), panel);
    const int sample_x = input.left + (input.right - input.left) / 4;
    Check(PtInRegion(dirty, sample_x,
                      readings.bottom + (input.top - readings.bottom) / 2),
          "live metrics still invalidate their display");
    Check(!RectInRegion(dirty, &input) &&
              !PtInRegion(dirty, materials.left + 2, materials.top + 2) &&
              !PtInRegion(dirty, readings.left + 1, readings.top + 1),
          "timer leaves the header, fixed materials and complete double track untouched");
  }
  DeleteObject(dirty);
  CHARRANGE replace{0, 1};
  SendMessageW(editor, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&replace));
  SendMessageW(editor, WM_CHAR, L'天', 0);
  Pump(host);
  Check(service->Practice()["metrics"]["attempts"] == 2 &&
            service->Practice()["metrics"]["corrections"] == 1,
        "native identical replacement is a new attempt");
  CheckCompositionDisplayGuards(host, service, panel, editor);
  CheckLivePracticeToggles(host, service, panel, editor);
  const auto textLength = GetWindowTextLengthW(editor);
  SendMessageW(editor, WM_PASTE, 0, 0);
  Pump(host);
  Check(GetWindowTextLengthW(editor) == textLength &&
            !service->Practice()["reasons"].empty(),
        "assisted paste blocked and marked");
  service->Finish("focusLost");
  Pump(host);
  Check(!service->Armed(), "practice disarmed");
  SendMessageW(panel, WM_COMMAND, MAKEWPARAM(7005, BN_CLICKED), 0);
  Pump(host);
  Check(service->Practice().empty() && GetWindowTextLengthW(editor) == 0,
        "changing article clears previous practice");
  const auto remembered_article = service->PracticePreferences().value("lastArticleID", "");
  Check(!remembered_article.empty() && remembered_article != metrics::Articles().front().id,
        "choosing another article remembers its stable identity");
  CheckIdleRefresh(host, panel);
  for (const int id : {7006, 7015, 7016}) PracticeButton(panel, id);
  Pump(host);
  const auto remembered_preferences = service->PracticePreferences();
  service->Save();
  {
    metrics::Service restored(storage);
    Check(restored.Issue().empty() &&
              restored.PracticePreferences() == remembered_preferences,
          "material, grid, readings and article choices survive a validated disk reload");
  }
  host.Close();
  host.OpenPage(nullptr, ui::SettingsPage::kTyping, 0, true);
  Pump(host);
  panel = FindWindowExW(host.hwnd(), nullptr, L"Rimes.ExtensionPages", nullptr);
  Check(!IsWindowVisible(GetDlgItem(panel, 7014)) &&
            service->PracticePreferences().value("lastArticleID", "") == remembered_article,
        "reopening practice restores its collapsed materials and last article");
  for (const int id : {7006, 7015, 7016}) PracticeButton(panel, id);
  Pump(host);
  if (typing_only) {
    // Keep the focused CI path independent of connector, gallery and other
    // settings pages. These renders contain only built-in text and fixture data.
    for (int t = 0; t < 4; ++t) {
      host.Close();
      service->ResetPractice();
      theme = static_cast<ui::ThemeId>(t);
      config.theme = ui::ThemeIdString(theme);
      host.OpenPage(nullptr, ui::SettingsPage::kTyping, 0, true);
      Pump(host);
      panel = FindWindowExW(host.hwnd(), nullptr, L"Rimes.ExtensionPages", nullptr);
      editor = GetDlgItem(panel, 7004);
      for (const auto language : {"chinese", "english"}) {
        const auto& articles = metrics::Articles();
        const auto found = std::find_if(articles.begin(), articles.end(), [&](const auto& a) { return a.language == language; });
        Check(found != articles.end(), "built-in materials cover both track languages");
        if (found == articles.end()) continue;
        const auto selected = static_cast<int>(std::distance(articles.begin(), found));
        SendMessageW(GetDlgItem(panel, 7000), CB_SETCURSEL, selected, 0);
        SendMessageW(panel, WM_COMMAND, MAKEWPARAM(7000, CBN_SELCHANGE), 0);
        Pump(host);
        const auto prefix = workbench::Wide(config.theme) + L"-typing-" + workbench::Wide(language) + L"-";
        Export(host.hwnd(), output / (prefix + L"ready.png"));
        PracticeButton(panel, 7001);
        Pump(host);
        Check(service->Armed() && GetFocus() == editor,
              "every skin starts both languages in the native editor");
        const auto characters = metrics::Characters(found->text);
        for (std::size_t i = 0; i < std::min<std::size_t>(2, characters.size()); ++i) {
          service->LocalKey('A' + static_cast<unsigned>(i), 0, false, GetTickCount(), false);
          for (const auto unit : characters[i]) SendMessageW(editor, WM_CHAR, unit, 0);
          Pump(host, 120);
        }
        Check(service->Practice()["metrics"]["committed"] == 2 &&
                  service->Practice()["metrics"]["correct"] == 2,
              "both languages retain and correctly score native committed text");
        Export(host.hwnd(), output / (prefix + L"live.png"));
        service->Finish("interrupted");
      }
    }
    host.Close();
    service->Save();
    return failures ? 1 : 0;
  }
  for (int t = 0; t < 4; ++t) {
    theme = static_cast<ui::ThemeId>(t);
    host.Close();
    config.theme = ui::ThemeIdString(theme); authorized = false; finish_login = false;
    for (auto page : {ui::SettingsPage::kTyping, ui::SettingsPage::kStatistics,
                      ui::SettingsPage::kPlugins, ui::SettingsPage::kApi})
      for (int sub = 0; sub < ui::SettingsTabCount(page); ++sub) {
        host.OpenPage(nullptr, page, sub, true);
        Pump(host);
        panel = FindWindowExW(host.hwnd(), nullptr, L"Rimes.ExtensionPages",
                              nullptr);
        const auto prefix = std::to_wstring(t) + L"-" +
                            std::to_wstring(static_cast<int>(page)) + L"-" +
                            std::to_wstring(sub);
        Export(host.hwnd(), output / (prefix + L"-top.png"));
        if (page == ui::SettingsPage::kPlugins || page == ui::SettingsPage::kStatistics) {
          const auto bar = GetDlgItem(panel, 7200);
          Check(bar && !(GetWindowLongPtrW(panel, GWL_STYLE) & WS_VSCROLL),
                "settings uses one native themed client scrollbar, without a duplicate system gutter");
          SCROLLINFO info{sizeof(info), SIF_ALL};
          Check(GetScrollInfo(bar, SB_CTL, &info) != FALSE, "accessible scrollbar retains native range");
          if (IsWindowVisible(bar)) {
            SendMessageW(panel, WM_VSCROLL, SB_BOTTOM, reinterpret_cast<LPARAM>(bar));
            GetScrollInfo(bar, SB_CTL, &info);
            Check(info.nPos == info.nMax - static_cast<int>(info.nPage) + 1,
                  "scrollbar can reach the last content row");
            SendMessageW(panel, WM_VSCROLL, SB_TOP, reinterpret_cast<LPARAM>(bar));
          }
        }
        SendMessageW(page == ui::SettingsPage::kApi ? host.hwnd() : panel, WM_VSCROLL, SB_PAGEDOWN, 0);
        Pump(host);
        Export(host.hwnd(), output / (prefix + L"-scroll.png"));
        if (page == ui::SettingsPage::kApi && sub == 0) {
          SendMessageW(GetDlgItem(host.hwnd(),6060), BM_CLICK, 0, 0);
          Pump(host);
          Check(IsWindowVisible(GetDlgItem(host.hwnd(), 6040)) && !IsWindowVisible(GetDlgItem(host.hwnd(), 6008)),
              "Codex uses independent login controls, without API key input");
          Pump(host,120);
          Export(host.hwnd(), output / (prefix + L"-codex.png"));
          SendMessageW(GetDlgItem(host.hwnd(),6040),BM_CLICK,0,0); Pump(host,120);
          Check(IsWindowVisible(GetDlgItem(host.hwnd(),6065)), "waiting login exposes explicit copy link");
          Export(host.hwnd(), output / (prefix + L"-waiting.png"));
          finish_login = true; Pump(host,150);
          Export(host.hwnd(), output / (prefix + L"-ready.png"));
          SendMessageW(GetDlgItem(host.hwnd(),6067),BM_CLICK,0,0); Pump(host);
          Export(host.hwnd(), output / (prefix + L"-advanced.png"));
          SendMessageW(GetDlgItem(host.hwnd(),6062), BM_CLICK, 0, 0);
        }
        if (page == ui::SettingsPage::kApi && sub == 1) {
          for (const auto id : {6052, 6053}) {
            SendMessageW(host.hwnd(), WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(host.hwnd(), id)));
            Pump(host);
            Export(host.hwnd(), output / (prefix + L"-client-" + std::to_wstring(id) + L".png"));
          }
        }
      }
  }
  // Minimum-size connector scrolling, disabled gateway and its retained client tab.
  host.OpenPage(nullptr,ui::SettingsPage::kApi,0,true);
  const auto dpi = GetDpiForWindow(host.hwnd());
  RECT compact{0,0,MulDiv(860,dpi,96),MulDiv(600,dpi,96)};
  AdjustWindowRectExForDpi(&compact,WS_OVERLAPPEDWINDOW,FALSE,0,dpi);
  SetWindowPos(host.hwnd(),nullptr,0,0,compact.right-compact.left,compact.bottom-compact.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
  SendMessageW(GetDlgItem(host.hwnd(),6002),BM_CLICK,0,0); Pump(host);
  Export(host.hwnd(),output/L"compact-api-top.png");
  SendMessageW(host.hwnd(),WM_VSCROLL,SB_BOTTOM,reinterpret_cast<LPARAM>(GetDlgItem(host.hwnd(),6070))); Pump(host);
  SCROLLINFO scroll{sizeof(scroll),SIF_ALL}; GetScrollInfo(GetDlgItem(host.hwnd(),6070),SB_CTL,&scroll);
  Check(scroll.nPos == scroll.nMax-static_cast<int>(scroll.nPage)+1 && scroll.nPos > 0, "connector scrollbar reaches final route at minimum size");
  Export(host.hwnd(),output/L"compact-api-bottom.png");
  host.OpenPage(nullptr,ui::SettingsPage::kApi,1,true);
  SendMessageW(GetDlgItem(host.hwnd(),6050),BM_CLICK,0,0); Pump(host);
  Check(!IsWindowEnabled(GetDlgItem(host.hwnd(),6056)), "disabled gateway cannot copy credentials");
  Export(host.hwnd(),output/L"compact-gateway-disabled.png");
  host.OpenPage(nullptr, ui::SettingsPage::kPlugins, 4, true);
  Pump(host);
  panel = FindWindowExW(host.hwnd(), nullptr, L"Rimes.ExtensionPages", nullptr);
  SendMessageW(panel, WM_COMMAND, MAKEWPARAM(8004, BN_CLICKED), 0);
  Pump(host);
  Check(!plugins[1].enabled,
        "gallery toggle controls exact registered extension");
  host.OpenPage(nullptr, ui::SettingsPage::kTyping, 0, true);
  Pump(host);
  Check(!IsWindowVisible(GetDlgItem(panel, 7004)),
        "disabled extension cannot open practice page");
  host.Close();
  service->Save();
  metrics::Service reloaded(storage);
  Check(reloaded.Issue().empty() &&
        reloaded.Data()["results"] == service->Data()["results"],
        "native practice results survive validated reload");
  return failures ? 1 : 0;
}
}  // namespace
int wmain(int argc, wchar_t** argv) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int result = 1;
  try {
    result = argc > 1 && std::wstring(argv[1]) == L"--typing-parity"
        ? Run(argc > 2 ? std::filesystem::path(argv[2])
                      : std::filesystem::temp_directory_path() /
                            (L"rimes-typing-parity-" + std::to_wstring(GetCurrentProcessId())),
              true)
        : argc > 1 && std::wstring(argv[1]) == L"--history-refresh"
        ? CheckHistoryRefresh(std::filesystem::temp_directory_path() /
                              (L"rimes-history-refresh-" + std::to_wstring(GetCurrentProcessId())))
        : argc > 2 && std::wstring(argv[2]) == L"--repaint-audit"
        ? AuditPages(std::filesystem::path(argv[1]))
        : Run(argc > 1 ? std::filesystem::path(argv[1])
                          : std::filesystem::temp_directory_path() /
                                (L"rimes-metrics-ui-" +
                                 std::to_wstring(GetCurrentProcessId())));
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
  }
  CoUninitialize();
  std::cout << "Native metric and gallery UI tests: "
            << (result ? "FAILED" : "PASS") << '\n';
  return result;
}
