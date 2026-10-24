#include "window.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <richedit.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <windowsx.h>

#include <atomic>
#include <fstream>
#include <future>
#include <map>

#include "../ui/theme.hpp"
#include "../ui/layered_frame.hpp"
#include "../workbench/codex_cli.hpp"
#include "../workbench/local_gateway.hpp"
#include "appearance.hpp"
#include "capture.hpp"
#include "media.hpp"
#include "interaction.hpp"
#include "messages.hpp"
#include "terminal.hpp"

namespace rimes::windows::peripherals {
using workbench::Utf8;
using workbench::Wide;
namespace {
std::string MessageTime() {
  SYSTEMTIME t{};
  GetLocalTime(&t);
  wchar_t text[32]{};
  swprintf_s(text, L"%04u-%02u-%02u %02u:%02u", t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute);
  return Utf8(text);
}
enum Control {
  Search = 100,
  Filter,
  List,
  Title,
  Summary,
  Body,
  Compose,
  Kind,
  Status,
  Route,
  New = 200,
  Save,
  Remove,
  Copy,
  Stage,
  Import,
  Archive,
  Send,
  Cancel,
  Reveal,
  Asset,
  Collect,
  Modules,
  Sidebar,
  Capture,
  Start,
  End,
  Receive,
  ToCapsule,
  Folder,
  Passcode,
  Paste,
  Preview,
  Manager,
  Back,
  Close,
  More,
  Cards,
  Scrim,
  CloseList,
  Heading,
  Subtitle,
  Edit,
  Inspector,
  CloseInspector,
  InspectorList,
  InspectorBody,
  FilterButton = 240,
  InspectorTab = 250,
  Nav = 300
};
std::wstring Text(HWND w) {
  const auto n = GetWindowTextLengthW(w);
  std::wstring value(static_cast<std::size_t>(n) + 1, L'\0');
  GetWindowTextW(w, value.data(), n + 1);
  value.resize(n);
  return value;
}
void Text(HWND w, const std::string& value) {
  SetWindowTextW(w, Wide(value).c_str());
}
std::wstring SelectFile(HWND owner) {
  wchar_t path[32768]{};
  OPENFILENAMEW open{sizeof(open)};
  open.hwndOwner = owner;
  open.lpstrFile = path;
  open.nMaxFile = 32768;
  open.lpstrFilter =
      L"文本、图片、视频和文档\0*.txt;*.md;*.json;*.png;*.jpg;*.jpeg;*.bmp;*."
      L"gif;*.mp4;*.webm;*.mov;*.pdf\0所有文件\0*.*\0";
  open.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  return GetOpenFileNameW(&open) ? path : L"";
}
std::wstring SelectFolder(HWND owner) {
  IFileOpenDialog* dialog = nullptr;
  std::wstring path;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                 CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&dialog)))) {
    dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                       FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    if (SUCCEEDED(dialog->Show(owner))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item))) {
        PWSTR value = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &value))) {
          path = value;
          CoTaskMemFree(value);
        }
        item->Release();
      }
    }
    dialog->Release();
  }
  return path;
}
std::string ClipboardText(HWND owner) {
  if (!OpenClipboard(owner)) return {};
  std::string text;
  if (auto data = GetClipboardData(CF_UNICODETEXT)) {
    const auto size = GlobalSize(data);
    if (size <= 512 * 1024)
      if (const auto* p = static_cast<const wchar_t*>(GlobalLock(data))) {
        const auto end = std::find(p, p + size / sizeof(wchar_t), L'\0');
        if (end != p + size / sizeof(wchar_t)) try {
            text = Utf8(std::wstring(p, end));
          } catch (...) {
          }
        GlobalUnlock(data);
      }
  }
  CloseClipboard();
  return text;
}
ClipboardWriteResult WriteClipboard(HWND owner, const std::string& value, bool secret) {
  const auto text = Wide(value);
  auto memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
  if (!memory) return {};
  auto* p = GlobalLock(memory);
  if (!p) {
    GlobalFree(memory);
    return {};
  }
  memcpy(p, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
  GlobalUnlock(memory);
  if (!OpenClipboard(owner)) {
    GlobalFree(memory);
    return {};
  }
  bool ok = EmptyClipboard() != FALSE;
  if (ok && secret) {
    for (auto name :
         {L"CanIncludeInClipboardHistory", L"CanUploadToCloudClipboard"}) {
      auto flag = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
      if (!flag || !SetClipboardData(RegisterClipboardFormatW(name), flag)) {
        if (flag) GlobalFree(flag);
        ok = false;
        break;
      }
    }
    auto marker = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
    if (!marker ||
        !SetClipboardData(RegisterClipboardFormatW(
                              L"ExcludeClipboardContentFromMonitorProcessing"),
                          marker)) {
      if (marker) GlobalFree(marker);
      ok = false;
    }
  }
  if (!ok || !SetClipboardData(CF_UNICODETEXT, memory)) {
    GlobalFree(memory);
    ok = false;
  }
  const ClipboardWriteResult result{ok, ok ? GetClipboardSequenceNumber() : 0};
  CloseClipboard();
  return result;
}
std::vector<std::pair<const char*, const wchar_t*>> Kinds(Module m) {
  switch (m) {
    case Module::Temporary:
      return {{"all", L"全部"},
              {"text", L"文本"},
              {"image", L"图片"},
              {"other", L"其他"}};
    case Module::Capture:
      return {{"all", L"全部"}, {"image", L"图片"}, {"video", L"视频"}};
    case Module::Notes:
      return {{"all", L"全部"},
              {"bullet", L"Bullet"},
              {"markdown", L"Markdown"},
              {"text", L"其他"}};
    case Module::Resources:
      return {{"all", L"全部"},      {"literature", L"文献"},
              {"document", L"文档"}, {"project", L"项目"},
              {"skill", L"Skills"},  {"other", L"其他"}};
    case Module::Passwords:
      return {{"all", L"全部"},
              {"login", L"登录"},
              {"key", L"密钥"},
              {"other", L"其他"}};
    case Module::Inbox:
      return {{"all", L"全部"},
              {"unread", L"未读"},
              {"pending", L"待处理"},
              {"archived", L"已归档"}};
    case Module::Chat:
      return {{"all", L"全部"}, {"archived", L"已归档"}};
    default:
      return {{"all", L"运行中的会话"}};
  }
}
COLORREF Color(std::uint32_t color) {
  return RGB((color >> 16) & 255, (color >> 8) & 255, color & 255);
}
}  // namespace
struct Windows::Impl {
  struct Pane;
  workbench::Runtime& runtime;
  workbench::Runtime::APIGenerator generate;
  Store store;
  ClipboardSource clipboard;
  ClipboardActions actions;
  ClipboardFeedback feedback;
  struct PendingPaste {
    workbench::Runtime::PeripheralPasteTicket ticket;
    PasteTarget target;
    DWORD sequence;
    ULONGLONG earliest, deadline;
  };
  std::optional<PendingPaste> pending_paste;
  std::unique_ptr<Pane> capsule, mailbox;
  std::unique_ptr<CaptureWindows> capture;
  struct Session {
    std::string id, title;
    std::wstring directory;
    std::unique_ptr<Terminal> process;
  };
  std::vector<Session> terminals;
  struct Job {
    std::string id, preview, error;
    Json record;
    bool done = false, success = false;
  };
  std::mutex job_mutex;
  Job job;
  std::atomic_bool cancel{false};
  std::jthread worker;
  std::string grant, connector_grant, connector;
  DWORD clipboard_sequence = 0, secret_sequence = 0;
  std::uint64_t secret_until = 0;
  std::uint64_t clipboard_retry = 0;
  unsigned clipboard_attempts = 0;
  std::uint64_t cleanup_due = 0;
  std::string clipboard_record;
  bool protected_session = false;
  std::optional<ui::ThemeId> preview_theme;
  ui::ThemeId CurrentTheme() const {
    return preview_theme.value_or(ui::ThemeIdOrDefault(
        workbench::EffectiveTheme(runtime.Configuration())));
  }
  unsigned long long gateway_revision = 0;
  Impl(workbench::Runtime& r, std::filesystem::path root,
       workbench::Runtime::APIGenerator g, ClipboardSource source, ClipboardActions writes)
      : runtime(r), generate(std::move(g)), store(std::move(root)),
        clipboard(std::move(source)), actions(std::move(writes)) {
    if (!actions.text) actions.text = WriteClipboard;
    if (!actions.asset) actions.asset = CopyCapsuleAsset;
  }
  ~Impl();
  void Open(Module, int mode = 0);
  void Generate(Pane&);
  void Tick();
  void Manage(Pane&);
  void SetEnabled(Module, bool);
  bool Authorized();
  void ClipboardChanged();
  void Cleanup();
  void Protect();
  void ClearSecretClipboard();
  void CaptureArea();
  CaptureWindows& Captures();
};
struct Windows::Impl::Pane {
  Impl& host;
  bool mail;
  HWND hwnd = nullptr;
  std::map<int, HWND> controls;
  HFONT font = nullptr, mono = nullptr;
  HBITMAP thumbnail = nullptr;
  std::map<std::string, HBITMAP> card_images;
  std::future<std::pair<std::string, HBITMAP>> card_image_job;
  HBRUSH background = nullptr, surface = nullptr;
  ui::ThemeId theme = ui::ThemeId::kNight;
  unsigned dpi = 96;
  Module module;
  Json rows = Json::array(), selected = Json::object();
  std::vector<std::string> terminal_ids;
  std::map<std::string, std::string> drafts;
  std::vector<std::pair<const char*, const wchar_t*>> kinds;
  std::string filter = "all";
  RevealGate gate;
  bool dirty = false, loading = false, unlocking = false, drawer = false;
  bool sidebar_hidden = false, composing = false, detail = false;
  bool borrowed = false, search_all = false, standalone_search = false;
  std::string borrowed_preedit;
  HWND opening_foreground = nullptr;
  PasteTarget opening_paste_target;
  OutsideClick outside_click;
  std::unique_ptr<MessageView> message_view;
  bool renaming = false, generating = false;
  bool inspector_hidden = false, inspector_drawer = false;
  int inspector_tab = 0;
  std::vector<std::pair<std::filesystem::path, std::string>> inspector_items;
  std::filesystem::path inspector_selection;
  std::string rendered_preview;
  std::string transcript_id;
  std::size_t unread_count = 0;
  int capsule_mode = 0;  // rail, manager, compact editor
  void ApplyWindowRole();
  int card_offset = 0, hover_card = -1, detail_row = 0;
  int pressed_card = -1, pressed_action = 0;
  HWND hover_control = nullptr;
  std::map<Module, std::string> filters;
  std::map<HWND, std::wstring> tooltips;
  HWND tooltip = nullptr;
  RECT editor_rect{}, composer_rect{};
  bool raw_keyboard = false;
  bool passcode_change = false;
  std::vector<std::string> passcode_first, passcode_confirm;
  std::set<unsigned> passcode_held, passcode_batch;
  std::uint64_t reveal_until = 0;
  std::wstring last_terminal;
  wchar_t terminal_surrogate = 0;
  Pane(Impl& h, bool m)
      : host(h),
        mail(m),
        module(m ? Module::Chat : Module::Temporary),
        gate(h.store.Root()) {}
  ~Pane() {
    if (card_image_job.valid()) {
      auto image = card_image_job.get().second;
      if (image) DeleteObject(image);
    }
    for (auto [id, image] : card_images)
      if (image) DeleteObject(image);
    if (hwnd) DestroyWindow(hwnd);
    if (thumbnail) DeleteObject(thumbnail);
    if (font) DeleteObject(font);
    if (mono) DeleteObject(mono);
    if (background) DeleteObject(background);
    if (surface) DeleteObject(surface);
  }
  HWND C(int id) {
    auto it = controls.find(id);
    return it == controls.end() ? nullptr : it->second;
  }
  void Set(int id, const std::string& value) { Text(C(id), value); }
  std::string Get(int id) { return Utf8(Text(C(id))); }
  void StatusText(const std::string& value) { Set(Status, value); }
  void Create();
  void Layout();
  void Paint(HDC);
  void DrawButton(DRAWITEMSTRUCT*);
  void DrawList(DRAWITEMSTRUCT*);
  void DrawCards(DRAWITEMSTRUCT*);
  void PaintCards(DRAWITEMSTRUCT*);
  void HoverCard(int);
  void Mode(int);
  void Borrow();
  void BorrowedInput();
  void CardImages();
  void RenderTranscript(const std::string& preview = "");
  void MessageAct(const MessageItem&, MessageAction);
  void ItemMenu();
  void SetFilter(std::size_t);
  void RefreshInspector();
  void SelectInspector(int);
  std::vector<std::wstring> DetailLines(bool copy = false);
  void Theme();
  void Open(Module, std::optional<int> mode = {});
  void Refresh();
  void Select(int index);
  void Load();
  void SaveRecord();
  bool Leave();
  void Lock();
  void Act(int);
  bool Key(MSG*);
  void Challenge(unsigned, bool);
  void ModulesMenu();
  void StoreDraft();
  Session* CurrentTerminal();
  std::string CopyText();
  void CopySelection(bool paste_image = false);
  static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR,
                                   DWORD_PTR);
  static LRESULT CALLBACK ControlProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR,
                                      DWORD_PTR);
};
Windows::Impl::~Impl() {
  runtime.ReleasePeripheral();
  cancel.store(true);
  if (worker.joinable()) worker.join();
  ClearSecretClipboard();
}
void Windows::Impl::Pane::Create() {
  WNDCLASSW wc{};
  wc.lpfnWndProc = Proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.style = CS_DBLCLKS;
  wc.lpszClassName = L"XSIME.Peripheral.Window";
  RegisterClassW(&wc);
  POINT pointer{};
  GetCursorPos(&pointer);
  MONITORINFO opening_monitor{sizeof(opening_monitor)};
  GetMonitorInfoW(MonitorFromPoint(pointer, MONITOR_DEFAULTTOPRIMARY),
                  &opening_monitor);
  hwnd = CreateWindowExW(
      WS_EX_COMPOSITED |
          (mail ? WS_EX_APPWINDOW
                : WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST),
      wc.lpszClassName, mail ? L"XSIME · Mailbox" : L"XSIME · Capsule",
      (mail ? WS_OVERLAPPEDWINDOW : WS_POPUP | WS_THICKFRAME) | WS_CLIPCHILDREN,
      mail ? CW_USEDEFAULT : opening_monitor.rcWork.left + 16,
      mail ? CW_USEDEFAULT : opening_monitor.rcWork.top + 16, 1000, 700,
      nullptr, nullptr, wc.hInstance, this);
  if (!hwnd) throw std::runtime_error("无法打开周边窗口。");
  auto make = [&](int id, const wchar_t* cls, const wchar_t* title,
                  DWORD style) {
    auto child = CreateWindowExW(
        0, cls, title, WS_CHILD | WS_TABSTOP | style, 0, 0, 1, 1, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), wc.hInstance,
        nullptr);
    controls[id] = child;
    SetWindowSubclass(child, ControlProc, 2, reinterpret_cast<DWORD_PTR>(this));
    if (std::wstring(cls) == L"EDIT")
      SendMessageW(child, EM_SETLIMITTEXT,
                   id == Body || id == Compose ? 256 * 1024 : 1024, 0);
  };
  make(Search, L"EDIT", L"", ES_AUTOHSCROLL);
  SendMessageW(C(Search), EM_SETCUEBANNER, TRUE,
               reinterpret_cast<LPARAM>(L"搜索标题与内容"));
  make(Filter, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL);
  make(List, L"LISTBOX", L"",
       LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
           WS_VSCROLL);
  make(Title, L"EDIT", L"", ES_AUTOHSCROLL);
  SendMessageW(C(Title), EM_SETCUEBANNER, TRUE,
               reinterpret_cast<LPARAM>(L"标题"));
  make(Summary, L"EDIT", L"", ES_AUTOHSCROLL);
  SendMessageW(C(Summary), EM_SETCUEBANNER, TRUE,
               reinterpret_cast<LPARAM>(L"摘要（可选）"));
  make(Kind, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL);
  make(Route, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL);
  if (mail) LoadLibraryW(L"Msftedit.dll");
  make(Body, mail ? MSFTEDIT_CLASS : L"EDIT", L"",
       ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL |
           WS_VSCROLL);
  SetWindowSubclass(C(Body), EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
  make(Compose, L"EDIT", L"",
       ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL);
  if (mail) {
    message_view = std::make_unique<MessageView>(
        hwnd, 237, [this](const MessageItem& item, MessageAction action) {
          try {
            MessageAct(item, action);
          } catch (const std::exception& e) {
            StatusText(e.what());
          }
        });
    controls[237] = message_view->Handle();
  }
  make(Status, L"STATIC", L"", SS_LEFT);
  make(Preview, L"STATIC", L"", SS_OWNERDRAW);
  make(Cards, L"STATIC", L"Capsule 内容", SS_OWNERDRAW | SS_NOTIFY);
  make(Scrim, L"STATIC", L"关闭列表", SS_OWNERDRAW | SS_NOTIFY);
  make(Heading, L"STATIC", L"", SS_OWNERDRAW);
  make(Subtitle, L"STATIC", L"", SS_OWNERDRAW);
  make(InspectorList, L"LISTBOX", L"",
       LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL);
  make(InspectorBody, L"EDIT", L"",
       ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL);
  for (auto [id, title] : std::vector<std::pair<int, const wchar_t*>>{
           {New, L"新建"},
           {Save, L"保存"},
           {Remove, L"删除"},
           {Copy, L"复制"},
           {Stage, L"加入 Buffer"},
           {Import, L"导入文件"},
           {Archive, L"归档"},
           {Send, L"发送"},
           {Cancel, L"停止"},
           {Reveal, L"验证 / 显示"},
           {Asset, L"打开附件"},
           {Modules, L"模块"},
           {Sidebar, L"列表"},
           {Capture, L"截图"},
           {Start, L"启动终端"},
           {End, L"结束"},
           {Receive, L"收取文件"},
           {ToCapsule, L"存入 Capsule"},
           {Folder, L"工作目录"},
           {Passcode, L"修改查看口令"},
           {Paste, L"从剪贴板"},
           {Manager, L"管理"},
           {Back, L"返回"},
           {Close, L"关闭"},
           {More, L"更多操作"},
           {CloseList, L"收起列表"},
           {Edit, L"编辑"},
           {Inspector, L"扩展区"},
           {CloseInspector, L"收起扩展区"}})
    make(id, L"BUTTON", title, BS_OWNERDRAW);
  for (int i = 0; i < 6; ++i)
    make(FilterButton + i, L"BUTTON", L"筛选", BS_OWNERDRAW);
  for (int i = 0; i < 3; ++i)
    make(InspectorTab + i, L"BUTTON",
         i == 0   ? L"产物"
         : i == 1 ? L"文件"
                  : L"会话",
         BS_OWNERDRAW);
  for (int i = mail ? 5 : 0; i < (mail ? 8 : 5); ++i)
    make(Nav + i, L"BUTTON", kModuleNames[i], BS_OWNERDRAW);
  AddClipboardFormatListener(hwnd);
  tooltip = CreateWindowExW(
      WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
      WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
      CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr, wc.hInstance, nullptr);
  for (auto [id, child] : controls)
    if (id >= New && id < FilterButton) {
      tooltips[child] = Text(child);
      TOOLINFOW info{sizeof(info)};
      info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
      info.hwnd = hwnd;
      info.uId = reinterpret_cast<UINT_PTR>(child);
      info.lpszText = tooltips[child].data();
      SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    }
  Theme();
  auto prefs = host.store.Preferences();
  auto size =
      prefs.value(mail ? "mailbox_client_size" : "capsule_manager_client_size",
                  mail ? Json::array({1000, 700}) : Json::array({940, 660}));
  if (size.is_array() && size.size() == 2 && size[0].is_number_integer() &&
      size[1].is_number_integer()) {
    RECT r{0, 0, MulDiv(std::clamp(size[0].get<int>(), 680, 1600), dpi, 96),
           MulDiv(std::clamp(size[1].get<int>(), 500, 1200), dpi, 96)};
    AdjustWindowRectExForDpi(
        &r, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), dpi);
    SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  if (!mail) {
    Mode(0);
    // WS_POPUP has no CW_USEDEFAULT placement. Match the Mac rail's first
    // summon: horizontally centred on the pointer screen, 24 DIP above its
    // work-area bottom. Later openings retain the user's moved position.
    RECT frame{};
    GetWindowRect(hwnd, &frame);
    const auto& work = opening_monitor.rcWork;
    const int width = frame.right - frame.left,
              height = frame.bottom - frame.top;
    const int left = work.left + (work.right - work.left - width) / 2;
    const int top =
        (std::max)(work.top, work.bottom - height - MulDiv(24, dpi, 96));
    SetWindowPos(hwnd, nullptr, left, top, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
}
void Windows::Impl::Pane::Theme() {
  const auto old_dpi = dpi;
  dpi = GetDpiForWindow(hwnd);
  if (!dpi) dpi = 96;
  theme = host.CurrentTheme();
  const auto& p = ui::Palette(theme);
  if (!font || old_dpi != dpi) {
    const auto old_font = font, old_mono = mono;
    font = CreateFontW(-MulDiv(12, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                       FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                       L"Microsoft YaHei UI");
    mono = CreateFontW(-MulDiv(13, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                       FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH,
                       L"Cascadia Mono");
    for (auto [id, child] : controls)
      SendMessageW(child, WM_SETFONT,
                   reinterpret_cast<WPARAM>(
                       id == Body && module == Module::Terminal ? mono : font),
                   FALSE);
    if (old_font) DeleteObject(old_font);
    if (old_mono) DeleteObject(old_mono);
  }
  // Module changes may swap the body between proportional and terminal fonts.
  const auto body_font = module == Module::Terminal ? mono : font;
  if (reinterpret_cast<HFONT>(SendMessageW(C(Body), WM_GETFONT, 0, 0)) !=
      body_font)
    SendMessageW(C(Body), WM_SETFONT, reinterpret_cast<WPARAM>(body_font),
                 FALSE);
  SendMessageW(C(List), LB_SETITEMHEIGHT, 0, MulDiv(mail ? 84 : 57, dpi, 96));
  if (background) DeleteObject(background);
  if (surface) DeleteObject(surface);
  const auto colors = appearance::Palette(theme, mail);
  background = CreateSolidBrush(Color(colors.background));
  surface = CreateSolidBrush(Color(colors.field));
  if (mail) {
    SendMessageW(
        C(Body), EM_SETBKGNDCOLOR, 0,
        Color(module == Module::Terminal ? colors.field : colors.background));
    CHARFORMAT2W format{sizeof(format)};
    format.dwMask = CFM_COLOR;
    format.crTextColor = Color(colors.text);
    SendMessageW(C(Body), EM_SETCHARFORMAT, SCF_ALL,
                 reinterpret_cast<LPARAM>(&format));
  }
  const BOOL dark = p.dark;
  DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                        sizeof(dark));
  if (mail && module != Module::Terminal) RenderTranscript(rendered_preview);
  RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}
void Windows::Impl::Pane::ApplyWindowRole() {
  if (!hwnd) return;
  const bool rail = !mail && capsule_mode == 0;
  if (!rail) outside_click.Stop();
  auto style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  // TOPMOST belongs to the native Z-order band. Change it with SetWindowPos,
  // not by overwriting GWL_EXSTYLE when entering search/editor/manager modes.
  style &= ~(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_APPWINDOW);
  style |= rail ? WS_EX_TOOLWINDOW | (standalone_search ? 0 : WS_EX_NOACTIVATE)
                : WS_EX_APPWINDOW;
  SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style);
  SetWindowPos(hwnd, rail ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
void Windows::Impl::Pane::Mode(int mode) {
  if (mail) return;
  const bool was_visible = IsWindowVisible(hwnd);
  capsule_mode = mode;
  standalone_search = false;
  ApplyWindowRole();
  if (mode != 0) {
    host.runtime.ReleasePeripheral();
    borrowed = false;
    Lock();
  } else if (!borrowed && was_visible) {
    if (GetForegroundWindow() == hwnd && IsWindow(opening_foreground) &&
        opening_foreground != hwnd)
      SetForegroundWindow(opening_foreground);
    Borrow();
  }
  detail = false;
  RECT old{};
  GetWindowRect(hwnd, &old);
  int w = mode == 0 ? 940 : mode == 1 ? 940 : 460;
  int h = mode == 0 ? 208 : mode == 1 ? 660 : 400;
  if (mode == 1) {
    auto size = host.store.Preferences().value("capsule_manager_client_size",
                                               Json::array({940, 660}));
    if (size.is_array() && size.size() == 2 && size[0].is_number_integer() &&
        size[1].is_number_integer()) {
      w = std::clamp(size[0].get<int>(), 620, 1600);
      h = std::clamp(size[1].get<int>(), 430, 1200);
    }
  }
  RECT r{0, 0, MulDiv(w, dpi, 96), MulDiv(h, dpi, 96)};
  AdjustWindowRectExForDpi(
      &r, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
      static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), dpi);
  MONITORINFO monitor{sizeof(monitor)};
  GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
  int width = (std::min)(r.right - r.left,
                         monitor.rcWork.right - monitor.rcWork.left - 24);
  int height = (std::min)(r.bottom - r.top,
                          monitor.rcWork.bottom - monitor.rcWork.top - 24);
  const int left =
      std::clamp(old.left + (old.right - old.left - width) / 2,
                 monitor.rcWork.left + 12, monitor.rcWork.right - width - 12);
  const int bottom = std::clamp(old.bottom, monitor.rcWork.top + height + 12,
                                monitor.rcWork.bottom - 12);
  SetWindowPos(hwnd, nullptr, left, bottom - height, width, height,
               SWP_NOZORDER | SWP_NOACTIVATE);
  Layout();
  if (mode != 0 && was_visible) SetForegroundWindow(hwnd);
  StatusText(mode == 0 ? "← → 选择 · Enter 插入或复制 · 双击查看 · Esc 关闭"
                       : "Ctrl+S 保存 · Esc 返回底栏");
}
void Windows::Impl::Pane::Layout() {
  RECT rect{};
  GetClientRect(hwnd, &rect);
  const int w = rect.right * 96 / static_cast<int>(dpi),
            h = rect.bottom * 96 / static_cast<int>(dpi);
  std::set<int> visible;
  auto place = [&](int id, int x, int y, int width, int height) {
    auto c = C(id);
    if (!c) return;
    visible.insert(id);
    SetWindowPos(
        c, nullptr, MulDiv(x, dpi, 96), MulDiv(y, dpi, 96),
        MulDiv(x + (std::max)(1, width), dpi, 96) - MulDiv(x, dpi, 96),
        MulDiv(y + (std::max)(1, height), dpi, 96) - MulDiv(y, dpi, 96),
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  };
  auto filters_at = [&](int x, int y, int width, bool vertical) {
    for (std::size_t i = 0; i < kinds.size(); ++i) {
      SetWindowTextW(C(FilterButton + static_cast<int>(i)), kinds[i].second);
      place(FilterButton + static_cast<int>(i),
            x + (vertical ? 0
                          : static_cast<int>(i) * width /
                                static_cast<int>(kinds.size())),
            y + (vertical ? static_cast<int>(i) * 21 : 0),
            vertical ? width : width / static_cast<int>(kinds.size()),
            vertical ? 18 : 24);
    }
  };
  SetRectEmpty(&editor_rect);
  SetRectEmpty(&composer_rect);
  if (mail) {
    const auto l = appearance::Mail(w, module, sidebar_hidden, drawer);
    const bool inspector =
        module == Module::Terminal &&
        (w - 64 >= 920 ? !inspector_hidden : inspector_drawer);
    const bool inspector_overlay = inspector && w - 64 < 920;
    const int right = w - (inspector && !inspector_overlay ? 240 : 0);
    int y = 64;
    for (int i = 5; i < 8; ++i)
      if (host.store.Enabled(static_cast<Module>(i))) {
        place(Nav + i, 8, y, 48, 56);
        y += 64;
      }
    place(Modules, 18, h - 40, 28, 28);
    const int x = l.main_left + 16, width = right - x - 16;
    const int action_width = module == Module::Terminal ? 96 : 128;
    place(renaming ? Title : Heading, x, 14, width - action_width - 12, 24);
    place(Subtitle, x, 42, width - action_width - 12, 18);
    place(Sidebar, right - action_width - 16, 20, 28, 28);
    if (module == Module::Terminal) {
      place(End, right - 80, 20, 28, 28);
      place(Inspector, right - 48, 20, 28, 28);
      place(Body, l.main_left, 76, right - l.main_left, h - 158);
      place(Compose, x, h - 72, width - 76, 34);
      place(Send, right - 84, h - 72, 68, 28);
      place(Status, x, h - 30, width, 18);
      if (auto* t = CurrentTerminal())
        t->process->Resize(
            static_cast<unsigned>((std::max)(20, (right - l.main_left) / 8)),
            static_cast<unsigned>((std::max)(5, (h - 158) / 17)));
    } else {
      place(renaming ? Save : Edit, w - 112, 20, 28, 28);
      place(Archive, w - 80, 20, 28, 28);
      place(More, w - 48, 20, 28, 28);
      place(237, l.main_left, 76, right - l.main_left, h - 234);
      composer_rect = {x, h - 148, x + width, h - 35};
      place(Compose, x + 10, h - 142, width - 20, 60);
      if (module == Module::Chat)
        place(Route, x + 10, h - 73, (std::min)(220, width - 120), 180);
      place(Send, w - 94, h - 73, 68, 28);
      // A single primary action toggles between Send and Stop while streaming.
      place(Status, x, h - 26, width, 18);
    }
    if (l.overlay || inspector_overlay) place(Scrim, 64, 0, w - 64, h);
    if (inspector) {
      place(CloseInspector, w - 44, 16, 28, 28);
      for (int i = 0; i < 3; ++i)
        place(InspectorTab + i, w - 224 + i * 69, 60, 69, 24);
      if (inspector_tab != 2)
        place(InspectorList, w - 224, 100, 208, (h - 180) / 2);
      place(thumbnail ? Preview : InspectorBody, w - 224,
            inspector_tab == 2 ? 100 : 100 + (h - 180) / 2 + 12, 208,
            inspector_tab == 2 ? h - 164 : (h - 180) / 2 - 8);
      place(Receive, w - 224, h - 44, 208, 28);
    }
    if (l.list_visible) {
      place(CloseList, 64 + l.list_width - 44, 16, 28, 28);
      if (module == Module::Terminal) {
        place(Route, 80, 60, l.list_width - 32, 180);
        place(Start, 80, 100, l.list_width - 32, 28);
        place(Folder, 80, h - 44, l.list_width - 32, 28);
        place(List, 64, 144, l.list_width, h - 200);
      } else {
        if (module == Module::Chat)
          place(New, 64 + l.list_width - 76, 16, 28, 28);
        place(Search, 84, 65, l.list_width - 40, 22);
        filters_at(80, 100, l.list_width - 32, false);
        place(List, 64, 140, l.list_width,
              h - 140 - (module == Module::Inbox ? 48 : 0));
        if (module == Module::Inbox) {
          place(Import, 80, h - 40, 88, 28);
          place(Paste, 176, h - 40, 104, 28);
        }
      }
    }
    if (l.overlay || inspector_overlay) {
      SetWindowPos(C(Scrim), HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      for (auto id :
           {List, Search, CloseList, New, Route, Start, Folder, Import, Paste})
        if (visible.contains(id) && (id != Route || module == Module::Terminal))
          SetWindowPos(C(id), HWND_TOP, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      for (std::size_t i = 0; i < kinds.size(); ++i)
        SetWindowPos(C(FilterButton + static_cast<int>(i)), HWND_TOP, 0, 0, 0,
                     0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      if (inspector_overlay)
        for (int id : std::initializer_list<int>{
                 CloseInspector, InspectorTab, InspectorTab + 1,
                 InspectorTab + 2, InspectorList, InspectorBody, Preview,
                 Receive})
          if (visible.contains(id))
            SetWindowPos(C(id), HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
  } else if (capsule_mode == 0) {
    int x = 116;
    for (int i = 0; i < 5; ++i)
      if (host.store.Enabled(static_cast<Module>(i))) {
        place(Nav + i, x, 14, 50, 26);
        x += 54;
      }
    const int search_width = (std::max)(0, w - x - 176);
    if (search_width >= 80) place(Search, x + 8, 17, search_width, 20);
    place(New, w - 132, 16, 22, 22);
    place(Manager, w - 102, 16, 54, 22);
    place(Close, w - 36, 16, 22, 22);
    if (w >= 750) place(Capture, w - 162, 16, 22, 22);
    filters_at(14, 53, 60, true);
    place(Cards, 84, 51, w - 98, 126);
    place(Status, 14, 185, w - 28, 16);
  } else {
    const bool editor = capsule_mode == 2;
    place(Manager, w - 72, 16, 22, 22);
    place(Close, w - 36, 16, 22, 22);
    if (editor) place(Back, 14, 16, 28, 22);
    int x = 12, width = w - 24, top = 52;
    if (!editor) {
      int y = 66;
      for (int i = 0; i < 5; ++i)
        if (host.store.Enabled(static_cast<Module>(i))) {
          place(Nav + i, 22, y, 102, 26);
          y += 29;
          if (i == static_cast<int>(module)) {
            filters_at(30, y, 86, true);
            y += static_cast<int>(kinds.size()) * 21 + 6;
          }
        }
      place(Modules, 24, h - 76, 98, 28);
      const int lw = std::clamp(w * 24 / 100, 170, 286);
      place(Search, 148, 60, lw - 46, 24);
      place(New, 142 + lw - 28, 58, 28, 28);
      place(List, 142, 98, lw, h - 136);
      x = 142 + lw + 27;
      width = w - x - 14;
      top = 56;
    }
    editor_rect = {x, top, x + width, h - 38};
    const int inner = x + 16, iw = width - 32;
    place(Title, inner, top + 35, iw, 26);
    if (module == Module::Passwords) {
      place(Reveal, inner, top + 74, 110, 28);
      place(Passcode, inner + 118, top + 74, iw - 118, 28);
    } else {
      place(Summary, inner, top + 74, iw, 24);
      place(Kind, inner, top + 108, (std::min)(140, iw), 180);
    }
    int body_top = top + (module == Module::Passwords ? 118 : 148);
    place(thumbnail ? Preview : Body, inner, body_top, iw, h - body_top - 100);
    place(Save, x + width - 80, h - 80, 64, 28);
    place(More, inner, h - 80, 28, 28);
    place(Copy, inner + 36, h - 80, 28, 28);
    if (module != Module::Passwords && module != Module::Resources &&
        (!selected.contains("asset") || selected.value("kind", "") == "text"))
      place(Stage, inner + 72, h - 80, 28, 28);
    place(Status, 14, h - 26, w - 28, 18);
  }
  // Hide only departed controls: hiding and showing the focused native edit on
  // every layout aborts IME composition and makes the first chord disappear.
  for (auto [id, c] : controls)
    if (!visible.contains(id) && (GetWindowLongPtrW(c, GWL_STYLE) & WS_VISIBLE))
      ShowWindow(c, SW_HIDE);
  if (!mail) EnableWindow(C(Capture), host.store.Enabled(Module::Capture));
  if (mail) {
    SendMessageW(C(Body), EM_SETREADONLY, TRUE, 0);
    RECT in{};
    GetClientRect(C(Body), &in);
    const int pad = MulDiv(module == Module::Terminal ? 12 : 0, dpi, 96);
    InflateRect(&in, -pad, -pad);
    SendMessageW(C(Body), EM_SETRECT, 0, reinterpret_cast<LPARAM>(&in));
  }
  InvalidateRect(hwnd, nullptr, FALSE);
}
void Windows::Impl::Pane::Paint(HDC dc) {
  using namespace appearance;
  appearance::Paint p(dc, dpi);
  RECT r{};
  GetClientRect(hwnd, &r);
  const int w = MulDiv(r.right, 96, dpi), h = MulDiv(r.bottom, 96, dpi);
  const auto c = Palette(theme, mail);
  p.Fill(r, c.background);
  if (mail) {
    const auto l = Mail(w, module, sidebar_hidden, drawer);
    p.Fill(p.R(0, 0, 64, h), c.sidebar);
    p.Text(L"M", p.R(8, 16, 48, 28), 22, c.accent, FW_SEMIBOLD,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (l.list_visible) {
      p.Fill(p.R(64, 0, l.list_width, h), c.sidebar);
      p.Text(module == Module::Terminal ? L"终端"
             : module == Module::Chat   ? L"对话"
                                        : L"收件箱",
             p.R(80, 16, l.list_width - 100, 28), 19, c.text, FW_SEMIBOLD);
      if (module != Module::Terminal) {
        p.Fill(p.R(80, 60, l.list_width - 32, 30), c.field, 6);
        p.Stroke(p.R(80, 60, l.list_width - 32, 30), c.border);
      }
      p.Fill(p.R(64 + l.list_width, 0, 1, h), c.border);
    }
    p.Fill(p.R(l.main_left, 75, w - l.main_left, 1), c.border);
    if (module == Module::Terminal &&
        (w - 64 >= 920 ? !inspector_hidden : inspector_drawer)) {
      p.Fill(p.R(w - 240, 0, 240, h), c.sidebar);
      p.Text(L"扩展区", p.R(w - 224, 16, 150, 28), 13, c.text, FW_SEMIBOLD);
    }
    if (!IsRectEmpty(&composer_rect)) {
      p.Fill(p.R(composer_rect.left, composer_rect.top,
                 composer_rect.right - composer_rect.left,
                 composer_rect.bottom - composer_rect.top),
             c.sidebar, 12);
    }
  } else {
    p.Text(capsule_mode == 2 ? L"编辑" : L"Capsule",
           p.R(capsule_mode == 2 ? 52 : 14, 12, 90, 30), 15, c.text, FW_BOLD,
           DT_SINGLELINE | DT_VCENTER, true);
    if (capsule_mode != 2)
      p.Text(std::to_wstring(rows.size()), p.R(94, 14, 32, 26), 10,
             c.secondary);
    if (capsule_mode == 0 && IsWindowVisible(C(Search))) {
      RECT search{};
      GetWindowRect(C(Search), &search);
      MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&search), 2);
      InflateRect(&search, p.S(4), p.S(4));
      p.Fill(search, c.field, 5);
    }
    if (capsule_mode == 1) {
      const int lw = std::clamp(w * 24 / 100, 170, 286);
      for (auto box : {p.R(14, 56, 118, h - 94), p.R(142, 98, lw, h - 136)}) {
        p.Fill(box, c.sidebar, 6);
        p.Stroke(box, c.border);
      }
      p.Fill(p.R(142 + lw + 12, 56, 1, h - 94), c.border);
      p.Fill(p.R(142, 56, lw - 36, 32), c.field, 6);
    }
    if (!IsRectEmpty(&editor_rect)) {
      auto e = editor_rect;
      p.Fill(p.R(e.left, e.top, e.right - e.left, e.bottom - e.top), c.sidebar,
             6);
      p.Text(module == Module::Passwords ? L"密码条目" : L"标题",
             p.R(e.left + 16, e.top + 8, e.right - e.left - 32, 18), 11,
             c.secondary, FW_NORMAL, DT_SINGLELINE | DT_VCENTER, true);
      for (auto id : {Title, Summary, Body})
        if (IsWindowVisible(C(id))) {
          RECT child{};
          GetWindowRect(C(id), &child);
          MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&child), 2);
          InflateRect(&child, p.S(4), p.S(4));
          p.Fill(child, c.field, 5);
        }
    }
  }
}
void Windows::Impl::Pane::DrawButton(DRAWITEMSTRUCT* d) {
  using namespace appearance;
  appearance::Paint p(d->hDC, dpi);
  const auto c = Palette(theme, mail);
  const int id = static_cast<int>(d->CtlID),
            w = MulDiv(d->rcItem.right, 96, dpi),
            h = MulDiv(d->rcItem.bottom, 96, dpi);
  const bool nav = id >= Nav && id < Nav + 8,
             filter_button = id >= FilterButton && id < FilterButton + 6;
  bool active = nav && id - Nav == static_cast<int>(module);
  if (id >= InspectorTab && id < InspectorTab + 3)
    active = id - InspectorTab == inspector_tab;
  if (filter_button)
    active = static_cast<std::size_t>(id - FilterButton) < kinds.size() &&
             filter == kinds[id - FilterButton].first;
  const bool hot = hover_control == d->hwndItem,
             pressed = (d->itemState & ODS_SELECTED) != 0;
  const bool disabled = (d->itemState & ODS_DISABLED) != 0;
  const bool primary = id == Send || id == Save;
  const bool icon_only =
      (mail && (id == Modules || id == New || id == Sidebar || id == Save ||
                id == Edit || id == Archive || id == More || id == End ||
                id == Inspector || id == CloseInspector || id == CloseList)) ||
      (!mail && (id == Close || id == More || id == Copy || id == Stage ||
                 id == Back || (id == Manager && capsule_mode != 0) ||
                 (capsule_mode == 0 && (id == New || id == Capture))));
  const auto base =
      mail ? ((nav || filter_button || id == Modules || id == New ||
               id == CloseList || id == CloseInspector || id >= InspectorTab ||
               id == Send)
                  ? c.sidebar
                  : c.background)
           : (capsule_mode == 0 || id == Manager || id == Close || id == Back
                  ? c.background
                  : c.sidebar);
  p.Fill(d->rcItem, base);
  const bool plain =
      nav || filter_button || icon_only || (id == Manager && capsule_mode == 0);
  if (primary && !disabled)
    p.Fill(d->rcItem, c.accent, 6);
  else if (active || pressed || hot)
    p.Fill(d->rcItem, c.selection, nav && mail ? 8 : 5);
  else if (!plain) {
    p.Fill(d->rcItem, c.field, 6);
    p.Stroke(d->rcItem, c.border);
  }
  const auto color = disabled ? c.secondary
                     : primary
                         ? (ui::Palette(theme).dark ? 0x1E2620U : 0xFFFFFFU)
                     : active ? c.accent
                              : c.text;
  Glyph glyph = Glyph::None;
  if (nav)
    glyph = ModuleGlyph(static_cast<Module>(id - Nav));
  else
    switch (id) {
      case New:
        glyph = mail ? Glyph::Edit : Glyph::Plus;
        break;
      case Start:
        glyph = Glyph::Plus;
        break;
      case Manager:
      case Modules:
        glyph = Glyph::Gear;
        break;
      case Close:
        glyph = Glyph::Close;
        break;
      case Back:
      case CloseList:
      case CloseInspector:
        glyph = Glyph::Back;
        break;
      case Sidebar:
        glyph = Glyph::Sidebar;
        break;
      case Inspector:
        glyph = Glyph::Inspector;
        break;
      case Edit:
      case Save:
        glyph = Glyph::Edit;
        break;
      case More:
        glyph = Glyph::More;
        break;
      case Copy:
        glyph = Glyph::Copy;
        break;
      case Stage:
        glyph = Glyph::Send;
        break;
      case Send:
        glyph = generating ? Glyph::Stop : Glyph::Send;
        break;
      case Archive:
      case Receive:
        glyph = Glyph::Archive;
        break;
      case End:
        glyph = Glyph::Stop;
        break;
      case Capture:
        glyph = Glyph::Camera;
        break;
      case Reveal:
        glyph = Glyph::Eye;
        break;
      case Folder:
        glyph = Glyph::Folder;
        break;
      default:
        break;
    }
  if (nav && mail) {
    Icon(p, glyph, p.R(15, 9, 18, 18), active ? c.accent : c.secondary);
    p.Text(Text(d->hwndItem), p.R(0, 33, 48, 16), 11,
           active ? c.accent : c.secondary, active ? FW_SEMIBOLD : FW_NORMAL,
           DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    if (id == Nav + static_cast<int>(Module::Inbox)) {
      const auto unread = unread_count;
      if (unread) {
        p.Fill(p.R(30, 5, 16, 14), c.accent, 7);
        p.Text(unread > 9 ? L"9+" : std::to_wstring(unread), p.R(30, 5, 16, 14),
               9, ui::Palette(theme).dark ? 0x1E2620 : 0xFFFFFF, FW_SEMIBOLD,
               DT_CENTER | DT_SINGLELINE | DT_VCENTER);
      }
    }
  } else if (icon_only)
    Icon(p, glyph, p.R((w - 18) / 2, (h - 18) / 2, 18, 18), color);
  else {
    const bool show_icon =
        glyph != Glyph::None && !(nav && !mail && capsule_mode == 0);
    if (show_icon) Icon(p, glyph, id == Manager
        ? p.R(5, (h - 14) / 2, 14, 14) : p.R(6, (h - 18) / 2, 18, 18), color);
    const int text_left = id == Manager ? 23 : show_icon ? 28 : 6;
    p.Text(Text(d->hwndItem), p.R(text_left, 0, w - text_left - 4, h),
           filter_button   ? (Text(d->hwndItem).size() > 6 ? 9 : 10)
           : id == Manager ? 11
                           : 12,
           color, active || primary ? FW_SEMIBOLD : FW_NORMAL,
           DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
               ((show_icon || (!mail && filter_button)) ? DT_LEFT : DT_CENTER));
  }
  if (d->itemState & ODS_FOCUS) {
    auto focus = d->rcItem;
    InflateRect(&focus, -2, -2);
    p.Stroke(focus, c.accent, 5);
  }
}
void Windows::Impl::Pane::DrawList(DRAWITEMSTRUCT* d) {
  using namespace appearance;
  appearance::Paint p(d->hDC, dpi);
  const auto c = Palette(theme, mail);
  p.Fill(d->rcItem, c.sidebar);
  if (d->itemID == static_cast<UINT>(-1)) return;
  RECT selected_rect = d->rcItem;
  InflateRect(&selected_rect, -p.S(mail ? 7 : 4), -p.S(3));
  if (d->itemState & ODS_SELECTED) p.Fill(selected_rect, c.selection, 7);
  const int x = MulDiv(d->rcItem.left, 96, dpi),
            y = MulDiv(d->rcItem.top, 96, dpi),
            width = MulDiv(d->rcItem.right - d->rcItem.left, 96, dpi);
  std::wstring title, summary, metadata;
  bool unread = false;
  if (d->itemID < rows.size()) {
    const auto& item = rows[d->itemID];
    title = Wide(item.value("title", ""));
    auto preview = item.value("summary", "");
    if (preview.empty()) preview = item.value("body", "");
    const auto messages = item.value("messages", Json::array());
    if (preview.empty() && !messages.empty())
      preview = messages.back().value("body", "");
    if (module == Module::Passwords) preview = "••••••••";
    summary = Wide(preview);
    std::replace(summary.begin(), summary.end(), L'\n', L' ');
    metadata = module == Module::Chat ? Wide(item.value("route", Json::object())
                                                 .value("model", "独立会话"))
               : module == Module::Inbox ? L"收件 · 本地"
                                         : Wide(item.value("kind", ""));
    if (item.value("archived", false)) metadata += L" · 已归档";
    unread = item.value("unread", false);
  } else {
    wchar_t value[1024]{};
    SendMessageW(d->hwndItem, LB_GETTEXT, d->itemID,
                 reinterpret_cast<LPARAM>(value));
    title = value;
    if (d->itemID < host.terminals.size())
      summary = host.terminals[d->itemID].directory;
    metadata = L"终端会话";
  }
  p.Text(title, p.R(x + 18, y + 9, width - 40, 20), mail ? 13 : 11, c.text,
         FW_SEMIBOLD);
  p.Text(summary, p.R(x + 18, y + (mail ? 31 : 29), width - 40, 19),
         mail ? 11 : 10, c.secondary);
  if (mail)
    p.Text(metadata, p.R(x + 18, y + 54, width - 40, 19), 10, c.secondary);
  if (unread) p.Fill(p.R(x + width - 18, y + 17, 5, 5), c.accent, 3);
  if (d->itemState & ODS_FOCUS) p.Stroke(selected_rect, c.accent, 7);
}
std::vector<std::wstring> Windows::Impl::Pane::DetailLines(bool copy) {
  std::string body = selected.value("body", "");
  if (module == Module::Passwords) {
    if (!gate.Authorized() || (!copy && !reveal_until))
      return {L"••••••••", L"••••••••", L"••••••••"};
    if (selected.contains("sealed")) body = Unseal(selected.at("sealed"));
  }
  std::vector<std::wstring> lines;
  std::size_t begin = 0;
  while (begin < body.size()) {
    auto end = body.find('\n', begin);
    if (end == std::string::npos) end = body.size();
    auto line = body.substr(begin, end - begin);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.starts_with("- ")) line.erase(0, 2);
    if (!line.empty()) lines.push_back(Wide(line));
    begin = end + 1;
  }
  Wipe(body);
  if (lines.empty()) lines.push_back(L"暂无内容");
  return lines;
}
void Windows::Impl::Pane::DrawCards(DRAWITEMSTRUCT* d) {
  // Keep the background clear and all card content in one completed frame.
  // The destination DC's clip region limits the final copy to damaged cards.
  ui::LayeredFrame frame;
  if (!frame.Resize(d->rcItem.right, d->rcItem.bottom)) {
    PaintCards(d);
    return;
  }
  auto buffered = *d;
  buffered.hDC = frame.dc();
  PaintCards(&buffered);
  BitBlt(d->hDC, 0, 0, d->rcItem.right, d->rcItem.bottom,
         frame.dc(), 0, 0, SRCCOPY);
}
void Windows::Impl::Pane::HoverCard(int index) {
  if (hover_card == index) return;
  const auto previous = hover_card;
  hover_card = index;
  if (detail || (module == Module::Passwords && !gate.Authorized())) return;
  appearance::Paint p(nullptr, dpi);
  RECT client{};
  GetClientRect(C(Cards), &client);
  for (const auto card : {previous, index}) {
    if (card < card_offset || card >= static_cast<int>(rows.size())) continue;
    // Hover changes only the copy glyph. Keep the preview/text/background
    // outside the damage region, including across rapid enter/leave cycles.
    const auto rect = p.R((card - card_offset) *
                             (appearance::kCardWidth + appearance::kCardGap) + 174,
                         98, 26, 26);
    RECT damage{};
    if (IntersectRect(&damage, &rect, &client))
      InvalidateRect(C(Cards), &damage, FALSE);
  }
}
void Windows::Impl::Pane::PaintCards(DRAWITEMSTRUCT* d) {
  using namespace appearance;
  appearance::Paint p(d->hDC, dpi);
  const auto c = Palette(theme, false);
  p.Fill(d->rcItem, c.background);
  const int w = MulDiv(d->rcItem.right, 96, dpi);
  if (detail && !selected.empty()) {
    p.Fill(p.R(0, 0, w, 126), c.sidebar, 8);
    Icon(p, Glyph::Back, p.R(10, 10, 14, 14), c.secondary);
    p.Text(L"返回", p.R(30, 6, 90, 22), 11, c.secondary);
    p.Text(Wide(selected.value("title", "")), p.R(12, 36, 182, 22), 13, c.text,
           FW_SEMIBOLD);
    p.Text(module == Module::Passwords ? L"空格显示 · Ctrl+C 复制"
                                       : L"↑↓ 选择 · Enter 加入 Buffer",
           p.R(12, 66, 182, 44), 10, c.secondary, FW_NORMAL, DT_WORDBREAK);
    if (module == Module::Passwords && !gate.Authorized()) {
      const int slot = (std::min)(56, (w - 246) / 4);
      for (int i = 0; i < 4; ++i) {
        const auto box = p.R(222 + i * (slot + 4), 20, slot, 32);
        p.Fill(box, c.field, 5);
        p.Stroke(box,
                 static_cast<std::size_t>(i) == gate.Progress() ? c.accent
                                                                : c.border,
                 5);
        p.Text(static_cast<std::size_t>(i) < gate.Progress() ? L"•••" : L"—",
               box, 12, c.secondary, FW_NORMAL,
               DT_CENTER | DT_SINGLELINE | DT_VCENTER);
      }
      p.Text(gate.Failed() ? L"验证失败，请重新输入四组并击"
             : gate.Held() ? L"输入中，松开全部按键后继续"
                           : L"依次输入四组物理并击",
             p.R(222, 68, w - 242, 40), 11, c.secondary, FW_NORMAL,
             DT_WORDBREAK);
      return;
    }
    auto lines = DetailLines();
    detail_row = std::clamp(detail_row, 0, static_cast<int>(lines.size()) - 1);
    const int first = (std::max)(0, detail_row - 3);
    for (int i = first;
         i < (std::min)(first + 4, static_cast<int>(lines.size())); ++i) {
      const int y = 10 + (i - first) * 26;
      if (i == detail_row) p.Fill(p.R(214, y, w - 224, 24), c.selection, 5);
      p.Text(lines[i], p.R(224, y, w - 244, 24), 12, c.text);
    }
    if (module == Module::Passwords)
      for (auto& line : lines)
        if (!line.empty())
          SecureZeroMemory(line.data(), line.size() * sizeof(wchar_t));
    return;
  }
  if (rows.empty()) {
    p.Text(Get(Search).empty() ? L"这里还没有内容" : L"没有找到匹配的内容",
           p.R(16, 38, w - 32, 24), 13, c.secondary, FW_NORMAL,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    const wchar_t* help =
        module == Module::Capture
            ? L"Alt+Shift+4 框选截图，完成后自动保存并显示左下角堆栈"
        : module == Module::Temporary
            ? (host.store.CollectClipboard()
                   ? L"复制文字、图片或文件后，会自动出现在这里"
                   : L"自动记录已暂停，点击下方开关恢复；已有历史仍会保留")
            : L"点击 + 新建，或打开管理导入内容";
    p.Text(help, p.R(16, 67, w - 32, 20), 11, c.secondary, FW_NORMAL,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return;
  }
  const int maximum = (std::max)(0, static_cast<int>(rows.size()) - 1);
  card_offset = std::clamp(card_offset, 0, maximum);
  for (int i = card_offset; i < static_cast<int>(rows.size()); ++i) {
    const int x = (i - card_offset) * (kCardWidth + kCardGap);
    if (x >= w) break;
    const auto& row = rows[i];
    const bool chosen = row.value("id", "") == selected.value("id", "");
    p.Fill(p.R(x, 0, kCardWidth, kCardHeight), chosen ? c.selection : c.sidebar,
           8);
    p.Stroke(p.R(x, 0, kCardWidth, kCardHeight), chosen ? c.accent : c.border,
             8);
    Icon(p, ModuleGlyph(module), p.R(x + 12, 11, 14, 14), c.secondary);
    p.Text(Wide(row.value("title", "")), p.R(x + 32, 8, 145, 22), 11, c.text,
           FW_SEMIBOLD);
    const auto preview =
        module == Module::Passwords
            ? L"••••••••"
            : Wide(row.value("body", row.value("summary", ""))).substr(0, 280);
    const auto asset = row.value("asset", "");
    if (card_images.contains(asset) && card_images[asset]) {
      auto bitmap = card_images[asset];
      BITMAP b{};
      GetObjectW(bitmap, sizeof(b), &b);
      const int width = (std::min)(182, MulDiv(62, b.bmWidth, b.bmHeight));
      const int height = MulDiv(width, b.bmHeight, b.bmWidth);
      auto source = CreateCompatibleDC(d->hDC);
      auto old = SelectObject(source, bitmap);
      SetStretchBltMode(d->hDC, HALFTONE);
      StretchBlt(d->hDC, p.S(x + 12 + (182 - width) / 2), p.S(34), p.S(width),
                 p.S(height), source, 0, 0, b.bmWidth, b.bmHeight, SRCCOPY);
      SelectObject(source, old);
      DeleteDC(source);
    } else
      p.Text(preview, p.R(x + 12, 38, 182, 56), 11, c.text, FW_NORMAL,
             DT_WORDBREAK | DT_END_ELLIPSIS);
    p.Text(Wide(row.value("kind", "")), p.R(x + 12, 102, 154, 18), 9,
           c.secondary);
    if (chosen) Icon(p, Glyph::More, p.R(x + 180, 10, 16, 16), c.text);
    if (hover_card == i && (module != Module::Passwords || gate.Authorized()))
      Icon(p, Glyph::Copy, p.R(x + 180, 103, 14, 14), c.text);
  }
}
void Windows::Impl::Pane::SetFilter(std::size_t index) {
  if (index >= kinds.size()) return;
  filter = kinds[index].first;
  filters[module] = filter;
  detail = false;
  card_offset = 0;
  Refresh();
  Layout();
}
void Windows::Impl::Pane::ItemMenu() {
  auto menu = CreatePopupMenu();
  auto add = [&](int id, const wchar_t* title, bool enabled = true) {
    AppendMenuW(menu, MF_STRING | (enabled ? 0 : MF_GRAYED), id, title);
  };
  if (!mail) {
    if (module != Module::Passwords && module != Module::Resources) {
      const bool asset = selected.contains("asset") && selected.value("kind", "") != "text";
      const bool image = asset && selected.value("kind", "") == "image";
      add(238,
          image ? L"粘贴图片到当前输入框\tEnter"
                : asset ? L"自动粘贴文件（尚未实现）"
                        : L"插入到当前输入框\tEnter",
          borrowed && !selected.empty() && (!asset || image));
    }
    add(Edit, selected.value("kind", "") == "image" ? L"编辑图片" : L"编辑条目",
        !selected.empty() && selected.value("kind", "") != "video");
    add(New, L"新建条目", module != Module::Capture);
  }
  add(Copy, L"复制\tCtrl+C",
      !selected.empty() && (module != Module::Passwords || gate.Authorized()));
  if (module != Module::Passwords && module != Module::Resources &&
      (!selected.contains("asset") || selected.value("kind", "") == "text"))
    add(Stage, L"加入 Buffer", !selected.empty());
  if (selected.contains("asset") || module == Module::Resources)
    add(Asset, L"打开附件", !selected.empty());
  if (module == Module::Temporary || module == Module::Inbox)
    add(ToCapsule, L"存入 Capsule", !selected.empty());
  if (module == Module::Capture)
    add(ToCapsule, selected.value("temporaryCapture", false) ? L"永久保留" : L"已永久保留",
        selected.value("temporaryCapture", false));
  if (module == Module::Passwords) {
    add(Reveal, L"验证 / 显示");
    add(Passcode, L"修改查看口令", gate.Authorized());
  }
  if (module != Module::Passwords && module != Module::Chat &&
      module != Module::Terminal) {
    add(Import, L"导入文件");
    if (module != Module::Resources) add(Paste, L"从剪贴板");
  }
  if (module == Module::Capture) {
    add(Capture, L"框选截图\tAlt+Shift+4");
    add(0, L"录屏（尚未实现）", false);
    add(0, L"OCR / 多图拼接（尚未实现）", false);
  }
  if (module == Module::Passwords)
    add(0, L"TOTP 动态验证码（尚未实现）", false);
  if (module == Module::Resources) add(0, L"文献引用排版（尚未实现）", false);
  if (!mail) add(0, L"云同步（尚未实现）", false);
  if (!mail) {
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(Remove, L"删除", !selected.empty());
  }
  POINT pt{};
  GetCursorPos(&pt);
  const int action = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x,
                                    pt.y, 0, hwnd, nullptr);
  DestroyMenu(menu);
  if (action) Act(action);
}
void Windows::Impl::Pane::RefreshInspector() {
  inspector_items.clear();
  inspector_selection.clear();
  SetWindowTextW(C(Receive), L"收取文件");
  SendMessageW(C(InspectorList), LB_RESETCONTENT, 0, 0);
  Set(InspectorBody, "");
  if (module != Module::Terminal) return;
  if (thumbnail) {
    DeleteObject(thumbnail);
    thumbnail = nullptr;
  }
  auto* terminal = CurrentTerminal();
  if (!terminal) {
    Set(InspectorBody,
        "启动终端后，可在这里查看产物、工作目录文件与会话状态。");
    return;
  }
  if (inspector_tab == 2) {
    Set(InspectorBody,
        terminal->title + "\r\n\r\n" +
            (terminal->process->Running() ? "运行中" : "已结束") + "\r\n\r\n" +
            Utf8(terminal->directory));
  } else if (inspector_tab == 0) {
    for (const auto& item : host.store.List(Module::Inbox))
      if (item.value("terminalSessionID", "") == terminal->id) {
        inspector_items.push_back({host.store.Asset(item), item.at("id")});
        auto title = Wide(item.value("title", ""));
        SendMessageW(C(InspectorList), LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(title.c_str()));
      }
    if (inspector_items.empty())
      Set(InspectorBody,
          "还没有收取的产物。点击下方按钮，选择本次会话的文件。");
  } else {
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator(terminal->directory, ec)) {
      if (inspector_items.size() >= 60) break;
      const auto attr = GetFileAttributesW(entry.path().c_str());
      if (attr == INVALID_FILE_ATTRIBUTES ||
          (attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        continue;
      inspector_items.push_back({entry.path(), ""});
      auto title = entry.path().filename().wstring();
      SendMessageW(C(InspectorList), LB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(title.c_str()));
    }
    Set(InspectorBody, inspector_items.empty()
                           ? "工作目录中没有文件。"
                           : "选择文件预览；点击“收取文件”才会保存到收件。");
  }
}
void Windows::Impl::Pane::SelectInspector(int index) {
  if (index < 0 || static_cast<std::size_t>(index) >= inspector_items.size())
    return;
  inspector_selection = inspector_items[index].first;
  SetWindowTextW(C(Receive), inspector_items[index].second.empty()
                                 ? L"收取文件"
                                 : L"打开收件");
  if (thumbnail) {
    DeleteObject(thumbnail);
    thumbnail = nullptr;
  }
  const auto size = std::filesystem::file_size(inspector_selection);
  if (size <= 64 * 1024 * 1024) thumbnail = Thumbnail(inspector_selection);
  if (!thumbnail && size <= 256 * 1024) {
    std::ifstream input(inspector_selection, std::ios::binary);
    std::string bytes(256 * 1024 + 1, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    if (bytes.size() <= 256 * 1024 && bytes.find('\0') == std::string::npos) {
      try {
        (void)Wide(bytes);
        Set(InspectorBody, bytes);
      } catch (...) {
        Set(InspectorBody, "此文件不提供文本预览。");
      }
    } else
      Set(InspectorBody, "此文件不提供文本预览。");
  } else if (!thumbnail)
    Set(InspectorBody, "此文件较大，可通过“收取文件”选择受支持的内容。");
  Layout();
}
void Windows::Impl::Pane::RenderTranscript(const std::string& preview) {
  if (!mail || !message_view || module == Module::Terminal) return;
  std::vector<MessageItem> items;
  if (selected.empty()) {
    items.push_back(
        {"empty", "empty",
         module == Module::Chat
             ? "# "
               "今天想一起做什么？\n\n继续思考、整理文字，或开始一项新任务。\n"
               "每个会话独立保留模型、上下文和草稿。"
             : "# 等待下一份收件\n\n从终端收取产物，或导入文字与图片。",
         "markdown"});
  } else {
    if (module == Module::Inbox) {
      MessageItem item{"receipt", "inbox", selected.value("body", ""),
                       selected.value("format", "plain")};
      if (selected.value("kind", "") == "image" && selected.contains("asset"))
        item.image = host.store.Asset(selected);
      item.review = selected.value("review", "pending") == "pending";
      items.push_back(std::move(item));
    }
    int index = 0;
    for (const auto& m : selected.value("messages", Json::array())) {
      const auto role = m.value("role", "assistant");
      items.push_back(
          {m.value("id", std::to_string(index++)), role, m.value("body", ""),
           m.value("format", role == "assistant" ? "markdown" : "plain"),
           m.value("time", "")});
    }
    if (!preview.empty())
      items.push_back(
          {"stream", "assistant", preview, "markdown", "", {}, true});
  }
  message_view->Update(selected.value("id", "empty"), std::move(items), theme,
                       dpi);
}
void Windows::Impl::Pane::MessageAct(const MessageItem& item,
                                     MessageAction action) {
  if (!host.store.Enabled(module) || selected.empty() || item.streaming) return;
  switch (action) {
    case MessageAction::Copy:
      if (!WriteClipboard(hwnd, item.body, false))
        throw std::runtime_error("剪贴板忙，请重试。");
      StatusText("已复制这条消息。");
      break;
    case MessageAction::Stage:
      if (module == Module::Inbox &&
          MessageBoxW(
              hwnd,
              (L"将这条消息加入 Buffer？\n\n" + Wide(item.body.substr(0, 2000)))
                  .c_str(),
              L"检查收件内容", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
      if (!host.runtime.StageExternal(item.body))
        throw std::runtime_error("Buffer 正在投递或容量不足，内容未改变。");
      if (module == Module::Inbox) {
        selected["review"] = "accepted";
        selected = host.store.Save(selected, selected.at("revision"));
      }
      StatusText("已加入 Buffer；在目标输入框重新绑定后发送。");
      break;
    case MessageAction::Save:
      if (!host.store.Enabled(Module::Notes))
        throw std::runtime_error("请先启用 Capsule 笔记。");
      host.store.Save(
          {{"module", "notes"},
           {"kind", item.format == "markdown" ? "markdown" : "text"},
           {"title", selected.value("title", "消息")},
           {"body", item.body}});
      StatusText("这条消息已存入 Capsule。");
      break;
    case MessageAction::OpenImage:
      if (std::filesystem::is_regular_file(item.image))
        ShellExecuteW(hwnd, L"open", item.image.c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
      break;
    case MessageAction::SaveImage: {
      wchar_t path[32768]{};
      wcscpy_s(path, item.image.filename().c_str());
      OPENFILENAMEW dialog{sizeof(dialog)};
      dialog.hwndOwner = hwnd;
      dialog.lpstrFile = path;
      dialog.nMaxFile = 32768;
      dialog.lpstrFilter = L"图片文件\0*.*\0\0";
      dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
      if (GetSaveFileNameW(&dialog) &&
          !CopyFileW(item.image.c_str(), path, FALSE))
        throw std::runtime_error("图片保存失败。");
      break;
    }
    case MessageAction::Ignore:
      if (module == Module::Inbox) {
        selected["review"] = "ignored";
        selected = host.store.Save(selected, selected.at("revision"));
        Refresh();
        Load();
      }
      break;
  }
}
bool Windows::Impl::Pane::Leave() {
  if (!dirty || mail) {
    StoreDraft();
    return true;
  }
  const auto choice = MessageBoxW(hwnd, L"保存当前条目的修改？", L"Capsule",
                                  MB_YESNOCANCEL | MB_ICONQUESTION);
  if (choice == IDCANCEL) return false;
  if (choice == IDYES) SaveRecord();
  dirty = false;
  return true;
}
void Windows::Impl::Pane::StoreDraft() {
  if (module == Module::Terminal && !selected.empty()) {
    drafts[selected.value("id", "")] = Get(Compose);
    return;
  }
  if (mail && module != Module::Terminal && !selected.empty()) {
    auto current = host.store.Get(selected.at("id"));
    const auto draft = Get(Compose);
    if (current.value("draft", "") != draft) {
      current["draft"] = draft;
      selected = host.store.Save(current, current.at("revision"));
    }
  }
}
void Windows::Impl::Pane::Lock() {
  if (!mail) host.runtime.PeripheralPhysical(false);
  if (raw_keyboard) {
    RAWINPUTDEVICE keyboard{1, 6, RIDEV_REMOVE, nullptr};
    RegisterRawInputDevices(&keyboard, 1, sizeof(keyboard));
    raw_keyboard = false;
  }
  gate.Reset();
  unlocking = false;
  passcode_change = false;
  passcode_first.clear();
  passcode_confirm.clear();
  passcode_held.clear();
  passcode_batch.clear();
  reveal_until = 0;
  if (C(Cards)) InvalidateRect(C(Cards), nullptr, TRUE);
  if (module == Module::Passwords) {
    loading = true;
    Set(Body, "");
    SendMessageW(C(Body), EM_SETREADONLY, TRUE, 0);
    loading = false;
    dirty = false;
  }
}
void Windows::Impl::Pane::Open(Module m, std::optional<int> mode) {
  if (!hwnd) Create();
  if (!Leave()) return;
  // Finish the role and dimensions before the first visible frame.
  if (!mail && mode) Mode(*mode);
  if (module == m && !selected.empty() && host.store.Enabled(m)) {
    Refresh();
    Load();
    Theme();
    Layout();
    ShowWindow(hwnd,
               !mail && capsule_mode == 0 ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_SHOWWINDOW);
    if (mail || capsule_mode != 0) SetForegroundWindow(hwnd);
    if (!mail) host.store.RememberCapsuleSelection(m);
    else host.store.RememberMailboxSelection(m);
    return;
  }
  if (!host.store.Enabled(m)) {
    StatusText("此模块已停用，可从“模块”重新启用。");
    ShowWindow(hwnd, SW_SHOW);
    return;
  }
  Lock();
  module = m;
  hover_card = -1;
  drawer = false;
  inspector_drawer = false;
  renaming = false;
  detail = false;
  card_offset = 0;
  selected = Json::object();
  filter = filters.contains(m) ? filters[m] : "all";
  loading = true;
  Set(Search, "");
  kinds = Kinds(m);
  SendMessageW(C(Filter), CB_RESETCONTENT, 0, 0);
  SendMessageW(C(Kind), CB_RESETCONTENT, 0, 0);
  for (auto [id, title] : kinds) {
    SendMessageW(C(Filter), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(title));
    if (std::string(id) != "all")
      SendMessageW(C(Kind), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(title));
  }
  for (std::size_t i = 0; i < kinds.size(); ++i)
    if (filter == kinds[i].first) SendMessageW(C(Filter), CB_SETCURSEL, i, 0);
  SendMessageW(C(Kind), CB_SETCURSEL, 0, 0);
  loading = false;
  Refresh();
  Load();
  Theme();
  Layout();
  ShowWindow(hwnd,
             !mail && capsule_mode == 0 ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
  SetWindowPos(
      hwnd, nullptr, 0, 0, 0, 0,
      SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  if (mail || capsule_mode != 0) SetForegroundWindow(hwnd);
  if (!mail) host.store.RememberCapsuleSelection(m);
  else host.store.RememberMailboxSelection(m);
}
void Windows::Impl::Pane::Refresh() {
  if (mail) unread_count = host.store.List(Module::Inbox, "", "unread").size();
  const auto old = selected.value("id", "");
  rows = Json::array();
  terminal_ids.clear();
  SendMessageW(C(List), LB_RESETCONTENT, 0, 0);
  if (module == Module::Terminal) {
    for (const auto& t : host.terminals) {
      terminal_ids.push_back(t.id);
      auto title =
          Wide(t.title + (t.process->Running() ? " · 运行中" : " · 已结束"));
      SendMessageW(C(List), LB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(title.c_str()));
    }
  } else {
    rows = host.store.List(module, Get(Search), filter);
    for (const auto& r : rows) {
      auto title =
          Wide((mail && r.value("unread", false) ? "● " : "") + r.value("title", ""));
      SendMessageW(C(List), LB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(title.c_str()));
    }
  }
  bool found = false;
  for (std::size_t i = 0; i < rows.size(); ++i)
    if (rows[i].at("id") == old) {
      found = true;
      selected = rows[i];
      SendMessageW(C(List), LB_SETCURSEL, i, 0);
    }
  if (!found && module != Module::Terminal) selected = Json::object();
  if (!found && !rows.empty()) {
    selected = rows.front();
    SendMessageW(C(List), LB_SETCURSEL, 0, 0);
  }
  InvalidateRect(hwnd, nullptr, FALSE);
  InvalidateRect(C(Cards), nullptr, TRUE);
}
void Windows::Impl::Pane::Select(int index) {
  if (!Leave()) return;
  Lock();
  detail_row = 0;
  dirty = false;
  if (module == Module::Terminal) {
    selected =
        index >= 0 && static_cast<std::size_t>(index) < terminal_ids.size()
            ? Json{{"id", terminal_ids[index]}}
            : Json::object();
  } else if (index >= 0 && static_cast<std::size_t>(index) < rows.size()) {
    selected = rows[index];
    if (mail && selected.value("unread", false)) {
      selected["unread"] = false;
      selected = host.store.Save(selected, selected.at("revision"));
      rows[index] = selected;
    }
  }
  Load();
  if (drawer) {
    drawer = false;
    Layout();
    SetFocus(mail ? C(Compose) : C(Cards));
  }
}
Windows::Impl::Session* Windows::Impl::Pane::CurrentTerminal() {
  const auto id = selected.value("id", "");
  for (auto& t : host.terminals)
    if (t.id == id) return &t;
  return nullptr;
}
void Windows::Impl::Pane::Load() {
  loading = true;
  dirty = false;
  generating = mail && module == Module::Chat && !selected.empty() &&
               selected.value("id", "") == host.job.id;
  rendered_preview.clear();
  Set(Title, selected.value("title", ""));
  Set(Summary, selected.value("summary", ""));
  Set(Compose, selected.value("draft", ""));
  if (thumbnail) {
    DeleteObject(thumbnail);
    thumbnail = nullptr;
  }
  if (selected.value("kind", "") == "image" && selected.contains("asset"))
    thumbnail = Thumbnail(host.store.Asset(selected));
  if (module == Module::Terminal) {
    RefreshInspector();
    SendMessageW(C(Route), CB_RESETCONTENT, 0, 0);
    EnableWindow(C(Route), TRUE);
    for (auto title : {L"PowerShell", L"Codex"})
      SendMessageW(C(Route), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(title));
    SendMessageW(C(Route), CB_SETCURSEL, 0, 0);
    SetWindowTextW(C(Send), L"输入");
    Set(Compose, drafts[selected.value("id", "")]);
    if (auto* t = CurrentTerminal()) {
      Set(Title, t->title);
      SetWindowTextW(C(Body), t->process->Output().c_str());
      StatusText(Utf8(t->directory));
    } else {
      Set(Body, "");
      StatusText("选择工作目录，开始会话。");
    }
  } else if (mail) {
    if (module == Module::Inbox && selected.value("unread", false)) {
      selected["unread"] = false;
      selected = host.store.Save(selected, selected.at("revision"));
      if (unread_count) --unread_count;
      for (auto& row : rows)
        if (row.value("id", "") == selected.value("id", "")) row = selected;
      InvalidateRect(C(List), nullptr, TRUE);
    }
    RenderTranscript();
    SetWindowTextW(C(Send), generating                ? L"停止"
                            : module == Module::Inbox ? L"保存"
                                                      : L"发送");
    StatusText(module == Module::Inbox
                   ? "本地备注 · Enter 保存，Shift+Enter 换行"
                   : "Enter 发送 · Shift+Enter 换行");
    SendMessageW(C(Route), CB_RESETCONTENT, 0, 0);
    auto s = host.runtime.Configuration();
    for (const auto& p : s.connections)
      SendMessageW(
          C(Route), CB_ADDSTRING, 0,
          reinterpret_cast<LPARAM>(Wide(p.name + " · " + p.model).c_str()));
    for (std::size_t i = 0; i < s.connections.size(); ++i)
      if (s.connections[i].id == selected.value("route", Json::object())
                                     .value("id", s.active_connection))
        SendMessageW(C(Route), CB_SETCURSEL, i, 0);
    EnableWindow(
        C(Route),
        selected.empty() || selected.value("messages", Json::array()).empty());
  } else if (module == Module::Passwords) {
    Set(Body, "");
    StatusText("验证四组物理并击后可编辑。复制不自动显示明文，失焦立即锁定。");
  } else {
    Set(Body, selected.value("body", ""));
    StatusText(capsule_mode == 0
                   ? (selected.contains("asset") &&
                              selected.value("kind", "") != "text"
                          ? "← → 选择 · Enter 复制 · 双击编辑图片 · Esc 关闭"
                          : "← → 选择 · Enter 插入当前输入框（未关联则复制） · "
                            "Esc 关闭")
                   : "Ctrl+S 保存 · Esc 返回底栏");
  }
  // Multiline Win32 edit controls do not support ES_PASSWORD. Locked records
  // contain no plaintext in the control; reveal is explicitly time limited.
  const bool file_group = ClipboardHasFiles(selected);
  SendMessageW(C(Body), EM_SETREADONLY,
               mail || file_group || (module == Module::Passwords && !gate.Authorized()), 0);
  EnableWindow(C(Kind), !file_group);
  if (file_group) SendMessageW(C(Kind), CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
  const auto kind = selected.value("kind", "text");
  for (std::size_t i = 1; i < kinds.size(); ++i)
    if (kind == kinds[i].first) SendMessageW(C(Kind), CB_SETCURSEL, i - 1, 0);
  loading = false;
  if (mail) {
    Set(Heading,
        selected.value("title", module == Module::Chat       ? "新对话"
                                : module == Module::Terminal ? "终端"
                                                             : "收件箱"));
    auto subtitle = selected.value("route", Json::object()).value("model", "");
    if (module == Module::Terminal)
      subtitle = CurrentTerminal() ? Utf8(CurrentTerminal()->directory)
                                   : "选择工作目录，开始新的会话";
    else if (subtitle.empty())
      subtitle =
          module == Module::Chat
              ? (selected.empty() ? "选择连接器，开始一个独立会话"
                                  : selected.value("route", Json::object())
                                        .value("id", "独立会话"))
              : "让完成的工作，有一个落点";
    Set(Subtitle, subtitle);
  }
  Layout();
}
void Windows::Impl::Pane::SaveRecord() {
  if (selected.empty()) throw std::runtime_error("请先新建或选择条目。");
  if (module == Module::Passwords && selected.contains("revision") &&
      !gate.Authorized())
    throw std::runtime_error("请先验证查看口令。");
  if (module == Module::Passwords && selected.contains("revision") &&
      !reveal_until)
    throw std::runtime_error("请先显示密码，再编辑并保存。");
  auto value = selected;
  value["title"] = Get(Title);
  if (!mail) {
    value["summary"] = Get(Summary);
    if (!ClipboardHasFiles(selected)) {
      value["body"] = Get(Body);
      const auto kind = SendMessageW(C(Kind), CB_GETCURSEL, 0, 0);
      if (kind >= 0 && static_cast<std::size_t>(kind + 1) < kinds.size())
        value["kind"] = kinds[kind + 1].first;
    }
  }
  selected = host.store.Save(value, value.value("revision", 0ULL));
  dirty = false;
  renaming = false;
  if (module == Module::Passwords) Lock();
  Refresh();
  StatusText("已保存到本机。");
  if (mail) Set(Heading, selected.value("title", ""));
  Layout();
}
std::string Windows::Impl::Pane::CopyText() {
  if (!mail && detail &&
      (module == Module::Notes || module == Module::Passwords)) {
    if (module == Module::Passwords &&
        (!gate.Authorized() || GetForegroundWindow() != hwnd))
      throw std::runtime_error("请先验证查看口令。");
    auto lines = DetailLines(true);
    auto result = Utf8(
        lines[std::clamp(detail_row, 0, static_cast<int>(lines.size()) - 1)]);
    for (auto& line : lines)
      if (!line.empty())
        SecureZeroMemory(line.data(), line.size() * sizeof(wchar_t));
    return result;
  }
  if (module == Module::Passwords) {
    if (!gate.Authorized() || GetForegroundWindow() != hwnd)
      throw std::runtime_error("请先验证查看口令。");
    return Unseal(host.store.Get(selected.at("id")).at("sealed"));
  }
  if (mail && !selected.empty()) {
    const auto messages = selected.value("messages", Json::array());
    for (auto it = messages.rbegin(); it != messages.rend(); ++it)
      if ((*it).value("role", "") == "assistant")
        return (*it).value("body", "");
    return selected.value("body", "");
  }
  return Get(Body);
}
void Windows::Impl::Pane::Challenge(unsigned key, bool down) {
  if (passcode_change && gate.Authorized()) {
    if (key < 'A' || key > 'Z') return;
    if (down) {
      passcode_held.insert(key);
      passcode_batch.insert(key);
      return;
    }
    if (!passcode_held.erase(key) || !passcode_held.empty()) return;
    std::string batch;
    for (auto k : passcode_batch) batch += static_cast<char>(k);
    passcode_batch.clear();
    if (passcode_first.size() < 4)
      passcode_first.push_back(batch);
    else
      passcode_confirm.push_back(batch);
    if (passcode_confirm.size() == 4) {
      gate.Change(passcode_first, passcode_confirm);
      Lock();
      StatusText("查看口令已更新。");
    } else
      StatusText(passcode_first.size() == 4
                     ? "再次输入相同四组并击以确认。"
                     : "输入新的四组并击；每组全部松开后继续。");
    return;
  }
  if (!unlocking) return;
  gate.Key(key, down);
  InvalidateRect(C(Cards), nullptr, TRUE);
  if (gate.Authorized()) {
    unlocking = false;
    if (raw_keyboard) {
      RAWINPUTDEVICE keyboard{1, 6, RIDEV_REMOVE, nullptr};
      RegisterRawInputDevices(&keyboard, 1, sizeof(keyboard));
      raw_keyboard = false;
    }
    StatusText("已验证 60 秒。点“验证 / 显示”可显示 15 秒；复制不延长查看期。");
    if (borrowed)
      host.runtime.PeripheralPhysical(false);
    else
      SetFocus(C(Reveal));
  } else
    StatusText(gate.Failed()
                   ? "验证失败，请重新输入四组并击。"
                   : std::string(gate.Progress() * 3, '*') +
                         (gate.Held() ? " · 输入中" : " · 等待下一组"));
}
void Windows::Impl::Pane::ModulesMenu() { host.Manage(*this); }
void Windows::Impl::Pane::CopySelection(bool paste_image) {
  const bool archive = !mail && !dirty && !detail && module != Module::Passwords && selected.contains("clipboard");
  const bool asset = !mail && selected.contains("asset") &&
                     selected.value("kind", "") != "text";
  const bool image = asset && selected.value("kind", "") == "image";
  const bool native_paste = image || (archive && ClipboardRequiresNativePaste(selected));
  const bool secret = module == Module::Passwords;
  const auto ticket = paste_image && native_paste
      ? host.runtime.PeripheralPasteTarget() : workbench::Runtime::PeripheralPasteTicket{};
  const auto target = opening_paste_target;
  host.pending_paste.reset();
  ClipboardWriteResult copied;
  if (archive) {
    copied = host.actions.archive(hwnd, host.store, selected);
  } else if (asset) {
    copied = host.actions.asset(hwnd, host.store, selected);
  } else {
    auto text = CopyText();
    if (text.empty()) return;
    copied = host.actions.text(hwnd, text, secret);
    Wipe(text);
  }
  if (!copied) throw std::runtime_error("剪贴板忙，请重试。");
  // Own writes are already represented by the selected card. Do not import
  // them again (including saved Notes/Resources and protected credentials).
  host.clipboard_sequence = copied.sequence;
  host.clipboard_retry = 0;
  host.clipboard_attempts = 0;
  host.clipboard_record = asset || archive ? selected.value("id", "") : "";
  if (secret) {
    host.secret_sequence = host.clipboard_sequence;
    host.secret_until = GetTickCount64() + 60000;
  }
  if (mail) { StatusText("已复制。"); return; }
  if (module == Module::Temporary && !selected.empty())
    selected = host.store.PromoteTemporary(selected);
  SendMessageW(hwnd, WM_CLOSE, 0, 0);
  if (IsWindowVisible(hwnd)) { StatusText("已复制。"); return; }
  if (paste_image && native_paste && ticket && target &&
      ticket.target.process == target.process && host.actions.target() == target &&
      host.runtime.CanPastePeripheral(ticket)) {
    const auto now = host.actions.now();
    host.pending_paste = PendingPaste{ticket, target, copied.sequence, now + 50, now + 900};
    return;
  }
  host.feedback.Show(secret ? L"已安全复制 · 60 秒后清除" : paste_image && native_paste
      ? L"已复制 · 请在目标输入框粘贴" : image ? L"已复制图片" : asset ? L"已复制文件" : L"已复制",
      theme, hwnd);
}
void Windows::Impl::Pane::Act(int id) {
  if (id >= InspectorTab && id < InspectorTab + 3) {
    inspector_tab = id - InspectorTab;
    RefreshInspector();
    Layout();
    return;
  }
  if (id >= FilterButton && id < FilterButton + 6) {
    SetFilter(static_cast<std::size_t>(id - FilterButton));
    return;
  }
  if (id >= Nav && id < Nav + 8) {
    Open(static_cast<Module>(id - Nav));
    return;
  }
  if (id != Modules && !host.store.Enabled(module)) {
    Lock();
    ShowWindow(hwnd, SW_HIDE);
    throw std::runtime_error("此模块已停用，内容仍保留在本机。");
  }
  switch (id) {
    case 238: {
      if (mail || module == Module::Passwords || module == Module::Resources)
        break;
      if (selected.contains("asset") && selected.value("kind", "") != "text") {
        CopySelection(selected.value("kind", "") == "image");
        break;
      }
      if (!dirty && !detail && ClipboardRequiresNativePaste(selected)) {
        CopySelection(true);
        break;
      }
      auto text = CopyText();
      if (borrowed) {
        if (!host.runtime.SendPeripheral(text))
          throw std::runtime_error(
              "请先完成组字，或重新唤出 Capsule 绑定输入框。");
        StatusText("正在插入…");
      } else {
        Act(Copy);
      }
      break;
    }
    case Sidebar:
    case CloseList: {
      RECT r{};
      GetClientRect(hwnd, &r);
      const auto l = appearance::Mail(MulDiv(r.right, 96, dpi), module,
                                      sidebar_hidden, drawer);
      if (l.narrow) {
        drawer = !drawer;
        inspector_drawer = false;
      } else
        sidebar_hidden = !sidebar_hidden;
      Layout();
      break;
    }
    case Scrim:
      drawer = false;
      inspector_drawer = false;
      Layout();
      SetFocus(C(Sidebar));
      break;
    case Inspector:
    case CloseInspector: {
      RECT r{};
      GetClientRect(hwnd, &r);
      if (MulDiv(r.right, 96, dpi) - 64 < 920) {
        inspector_drawer = !inspector_drawer;
        drawer = false;
      } else
        inspector_hidden = !inspector_hidden;
      RefreshInspector();
      Layout();
      break;
    }
    case Manager:
      if (Leave()) Mode(capsule_mode == 1 ? 0 : 1);
      break;
    case Back:
      if (detail) {
        detail = false;
        InvalidateRect(C(Cards), nullptr, TRUE);
      } else if (Leave())
        Mode(0);
      break;
    case Close:
      PostMessageW(hwnd, WM_CLOSE, 0, 0);
      break;
    case More:
      ItemMenu();
      break;
    case Edit:
      if (selected.empty()) break;
      if (!mail && selected.value("kind", "") == "image" &&
          selected.contains("asset")) {
        if (Leave()) host.Captures().Edit(selected);
        break;
      }
      if (!mail && selected.value("kind", "") == "video")
        throw std::runtime_error("视频编辑尚未实现；可使用打开附件播放。");
      if (mail) {
        renaming = true;
        Layout();
        SetFocus(C(Title));
        SendMessageW(C(Title), EM_SETSEL, 0, -1);
      } else {
        Mode(2);
        Load();
        SetFocus(C(Title));
      }
      break;
    case Modules:
      ModulesMenu();
      break;
    case Collect:
      // Recording is a settings preference, never a quick-rail action.
      break;
    case New:
      if (module == Module::Capture) {
        host.CaptureArea();
        break;
      }
      if (!Leave()) break;
      if (!mail) Mode(2);
      Lock();
      selected = {{"module", kModuleIds[static_cast<unsigned>(module)]},
                  {"title", module == Module::Chat ? "新对话" : "新条目"},
                  {"body", ""},
                  {"kind", !mail && filter != "all"  ? filter
                           : module == Module::Notes ? "bullet"
                                                     : "text"}};
      if (module == Module::Terminal) {
        Act(Start);
        break;
      }
      if (mail) {
        if (module == Module::Chat) {
          auto config = host.runtime.Configuration();
          auto choice = SendMessageW(C(Route), CB_GETCURSEL, 0, 0);
          if (choice >= 0 &&
              static_cast<std::size_t>(choice) < config.connections.size())
            workbench::SelectConnection(&config, config.connections[choice].id);
          selected["route"] = FreezeRoute(config);
        }
        selected["messages"] = Json::array();
        selected = host.store.Save(selected);
        Refresh();
      }
      Load();
      if (module == Module::Passwords) {
        SendMessageW(C(Body), EM_SETREADONLY, FALSE, 0);
        Set(Body, "- 用户名：\r\n- 密码：\r\n- 网址：");
      }
      drawer = false;
      Layout();
      SetFocus(mail ? C(Compose) : C(Title));
      break;
    case Save:
      SaveRecord();
      break;
    case Remove:
      if (selected.empty()) break;
      if (module == Module::Passwords && !gate.Authorized())
        throw std::runtime_error("删除密码前须验证查看口令。");
      if (MessageBoxW(hwnd, L"删除此条目？原始文件会保留。", L"Capsule",
                      MB_YESNO | MB_ICONQUESTION) == IDYES) {
        host.store.Erase(selected.at("id"), selected.at("revision"));
        Lock();
        selected = Json::object();
        Refresh();
        Load();
      }
      break;
    case Copy: {
      CopySelection();
      break;
    }
    case Stage: {
      if (module == Module::Passwords || module == Module::Resources) break;
      auto text = CopyText();
      if (text.empty()) throw std::runtime_error("没有可暂存的文本。");
      if (mail && module == Module::Inbox) {
        const auto preview = Wide(text.substr(0, 2000));
        if (MessageBoxW(hwnd,
                        (L"将下面的内容暂存到 Buffer？\n\n" + preview).c_str(),
                        L"检查收件内容", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
          break;
      }
      if (!host.runtime.StageExternal(text))
        throw std::runtime_error("Buffer 正在投递或容量不足，内容未改变。");
      if (!mail) {
        host.runtime.ReleasePeripheral();
        borrowed = false;
      }
      if (module == Module::Inbox && !selected.empty()) {
        selected["review"] = "accepted";
        selected = host.store.Save(selected, selected.at("revision"));
      }
      ShowWindow(hwnd, SW_HIDE);
      break;
    }
    case Import:
    case Receive: {
      if (id == Receive && inspector_tab == 0 && !inspector_selection.empty()) {
        std::string receipt;
        for (const auto& item : inspector_items)
          if (item.first == inspector_selection) receipt = item.second;
        if (!receipt.empty() && host.store.Enabled(Module::Inbox)) {
          auto record = host.store.Get(receipt);
          Open(Module::Inbox);
          selected = record;
          Refresh();
          Load();
          break;
        }
      }
      if (id == Import &&
          (module == Module::Passwords || module == Module::Chat ||
           module == Module::Terminal))
        break;
      auto path = id == Receive && !inspector_selection.empty()
                      ? inspector_selection.wstring()
                      : SelectFile(hwnd);
      if (path.empty()) break;
      const auto destination = id == Receive ? Module::Inbox : module;
      if (!host.store.Enabled(destination))
        throw std::runtime_error("请先启用收件模块。");
      const auto terminal =
          id == Receive && CurrentTerminal() ? CurrentTerminal()->id : "";
      auto item = host.store.Import(destination, path, terminal);
      if (destination == module) {
        selected = item;
        Refresh();
        Load();
      } else
        StatusText("文件已收取到收件工作区。");
      if (module == Module::Terminal) {
        RefreshInspector();
        Layout();
      }
      break;
    }
    case Paste: {
      if (module == Module::Passwords || module == Module::Resources ||
          module == Module::Terminal || module == Module::Chat)
        break;
      if (!Leave()) break;
      auto content = ReadClipboard(hwnd);
      Json item;
      if (!mail && (!content.formats.empty() || !content.files.empty()))
        item = host.store.RecordClipboard(content, module);
      else if (!content.bytes.empty())
        item = host.store.ImportData(module, "剪贴板图片", content.extension,
                                     content.bytes);
      else if (!content.files.empty()) {
        for (const auto& file : content.files) item = host.store.Import(module, file);
      }
      else if (!content.text.empty())
        item = host.store.Save(
            {{"module", kModuleIds[static_cast<unsigned>(module)]},
             {"kind", "text"},
             {"title", "剪贴板文字"},
             {"body", content.text},
             {"messages", Json::array()},
             {"review", "pending"}});
      else
        throw std::runtime_error("剪贴板中没有可导入的内容。");
      selected = item;
      Refresh();
      Load();
      break;
    }
    case Archive:
      if (selected.empty()) break;
      selected["archived"] = !selected.value("archived", false);
      selected = host.store.Save(selected, selected.at("revision"));
      selected = Json::object();
      Refresh();
      Load();
      break;
    case Send:
      if (generating) {
        host.cancel.store(true);
        break;
      }
      if (module == Module::Terminal) {
        if (auto* t = CurrentTerminal()) {
          const auto text = Get(Compose);
          if (!text.empty() && t->process->Running()) {
            if (t->process->Input(text + "\r"))
              Set(Compose, "");
            else
              StatusText("终端输入超过 64 KiB 或当前通道已关闭，内容已保留。");
          }
        }
        break;
      }
      if (module == Module::Chat) {
        host.Generate(*this);
        break;
      }
      if (module == Module::Inbox && !selected.empty() &&
          !Get(Compose).empty()) {
        selected["messages"].push_back({{"id", NewId()},
                                        {"time", MessageTime()},
                                        {"role", "note"},
                                        {"body", Get(Compose)}});
        selected["draft"] = "";
        selected = host.store.Save(selected, selected.at("revision"));
        Load();
      }
      break;
    case Cancel:
      host.cancel.store(true);
      break;
    case Reveal:
      if (selected.empty() || !selected.contains("sealed")) break;
      if (gate.Authorized()) {
        reveal_until = GetTickCount64() + 15000;
        auto plain = Unseal(host.store.Get(selected.at("id")).at("sealed"));
        loading = true;
        Set(Body, plain);
        Wipe(plain);
        loading = false;
        SendMessageW(C(Body), EM_SETREADONLY, FALSE, 0);
        InvalidateRect(C(Cards), nullptr, TRUE);
      } else {
        Lock();
        if (borrowed) {
          host.runtime.PeripheralPhysical(true);
          unlocking = true;
          StatusText("按顺序输入四组物理并击，全部松开一组后继续。Esc 取消。");
          break;
        }
        if (!mail && capsule_mode == 0) Mode(2);
        RAWINPUTDEVICE keyboard{1, 6, 0, hwnd};
        raw_keyboard =
            RegisterRawInputDevices(&keyboard, 1, sizeof(keyboard)) != FALSE;
        if (!raw_keyboard)
          throw std::runtime_error("无法读取物理并击，请重试。");
        unlocking = true;
        SetFocus(hwnd);
        StatusText("按顺序输入四组物理并击，全部松开一组后继续。Esc 取消。");
      }
      break;
    case Passcode:
      if (!gate.Authorized())
        throw std::runtime_error("请先验证当前查看口令，再修改。");
      passcode_change = true;
      {
        RAWINPUTDEVICE keyboard{1, 6, 0, hwnd};
        raw_keyboard =
            RegisterRawInputDevices(&keyboard, 1, sizeof(keyboard)) != FALSE;
        if (!raw_keyboard) {
          passcode_change = false;
          throw std::runtime_error("无法读取物理并击，请重试。");
        }
      }
      passcode_first.clear();
      passcode_confirm.clear();
      SetFocus(hwnd);
      StatusText("输入新的四组物理并击，然后再输入一次确认。");
      break;
    case Asset: {
      if (selected.empty()) break;
      const auto path = module == Module::Resources
                            ? Wide(selected.value("body", ""))
                            : host.store.Asset(selected).wstring();
      if (!std::filesystem::exists(path))
        throw std::runtime_error("附件或资源暂时不可用。");
      if (reinterpret_cast<INT_PTR>(ShellExecuteW(
              hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <=
          32)
        throw std::runtime_error("无法打开附件。");
      break;
    }
    case Capture:
      if (Leave()) host.CaptureArea();
      break;
    case ToCapsule: {
      if (selected.empty()) break;
      if (module == Module::Capture) {
        selected["temporaryCapture"] = false;
        selected = host.store.Save(selected, selected.at("revision"));
        Refresh();
        StatusText("已永久保留，不参与自动清理。");
        break;
      }
      Json value;
      if (!host.store.Enabled(selected.contains("asset") ? Module::Capture
                                                         : Module::Notes))
        throw std::runtime_error("请先在 Capsule 启用目标模块。");
      if (!mail && selected.contains("clipboard") && !dirty && !detail) {
        value = selected;
        value.erase("id"); value.erase("revision"); value.erase("promoted");
        value["module"] = selected.contains("asset") ? "capture" : "notes";
        value = host.store.Save(value);
      } else if (selected.contains("asset"))
        value = host.store.Import(Module::Capture, host.store.Asset(selected));
      else {
        auto text = CopyText();
        if (text.empty()) break;
        value = host.store.Save({{"module", "notes"},
                                 {"kind", "markdown"},
                                 {"title", selected.value("title", "收件内容")},
                                 {"body", text}});
      }
      StatusText("已独立保存到 Capsule。");
      break;
    }
    case Folder: {
      auto path = SelectFolder(hwnd);
      if (!path.empty()) {
        drafts["terminal_directory"] = Utf8(path);
        StatusText(Utf8(path));
      }
      break;
    }
    case Start: {
      auto directory = drafts["terminal_directory"];
      if (directory.empty()) {
        auto path = SelectFolder(hwnd);
        if (path.empty()) break;
        directory = Utf8(path);
        drafts["terminal_directory"] = directory;
      }
      wchar_t system[MAX_PATH]{};
      GetSystemDirectoryW(system, MAX_PATH);
      auto terminal = std::make_unique<Terminal>();
      const bool codex = SendMessageW(C(Route), CB_GETCURSEL, 0, 0) == 1;
      if (codex) {
        bool enabled = false;
        for (const auto& p : host.runtime.Plugins())
          if (p.id == "builtin.codex-cli" && p.enabled && p.installed)
            enabled = true;
        if (!enabled) throw std::runtime_error("请先启用 Codex 连接器。");
        terminal->Start(
            workbench::ResolveCodex(host.runtime.Configuration().codex_path)
                .wstring(),
            L"", Wide(directory));
      } else
        terminal->Start(
            std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe",
            L"-NoLogo", Wide(directory));
      Session t{NewId(),
                std::string(codex ? "Codex " : "PowerShell ") +
                    std::to_string(host.terminals.size() + 1),
                Wide(directory), std::move(terminal)};
      selected = {{"id", t.id}};
      host.terminals.push_back(std::move(t));
      Refresh();
      Load();
      SetFocus(C(Body));
      break;
    }
    case End:
      if (auto* t = CurrentTerminal()) {
        t->process->End();
        Refresh();
        StatusText("选中的终端已结束。");
      }
      break;
  }
}
bool Windows::Impl::Pane::Key(MSG* m) {
  if (!hwnd || !IsWindowVisible(hwnd) ||
      (m->hwnd != hwnd && !IsChild(hwnd, m->hwnd)))
    return false;
  if (composing && (m->hwnd == C(Compose) || m->hwnd == C(Search) ||
                    m->hwnd == C(Body) || m->hwnd == C(Title)))
    return false;
  if (m->message == WM_KEYDOWN && m->hwnd == C(Compose) &&
      m->wParam == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000)) {
    Act(Send);
    return true;
  }
  if (m->message == WM_KEYDOWN && renaming && m->hwnd == C(Title) &&
      m->wParam == VK_RETURN) {
    Act(Save);
    SetFocus(C(Edit));
    return true;
  }
  if (m->message == WM_KEYDOWN && m->wParam == VK_ESCAPE) {
    if (module == Module::Terminal && m->hwnd == C(Body)) {
      if (auto* t = CurrentTerminal()) t->process->Input("\x1b");
      return true;
    }
    if (unlocking || passcode_change) {
      Lock();
      StatusText("已取消验证。");
    } else if (renaming) {
      renaming = false;
      Set(Title, selected.value("title", ""));
      Layout();
      SetFocus(C(Edit));
    } else if (detail) {
      detail = false;
      InvalidateRect(C(Cards), nullptr, TRUE);
    } else if (!mail && capsule_mode != 0) {
      if (Leave()) Mode(0);
    } else if (drawer || inspector_drawer) {
      drawer = false;
      inspector_drawer = false;
      Layout();
    } else
      PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return true;
  }
  if (unlocking || passcode_change) {
    // WM_INPUT observes native physical batches even when TSF consumes the
    // legacy key messages. Never accept translated IME text as a credential.
    return m->message == WM_KEYDOWN || m->message == WM_KEYUP ||
           m->message == WM_CHAR;
  }
  if (m->message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
    if (m->wParam == 'F') {
      if (mail) {
        RECT r{};
        GetClientRect(hwnd, &r);
        if (appearance::Mail(MulDiv(r.right, 96, dpi), module, sidebar_hidden,
                             drawer)
                .narrow)
          drawer = true;
        else
          sidebar_hidden = false;
        Layout();
      }
      SetFocus(C(Search));
      return true;
    }
    if (m->wParam == 'N') {
      Act(New);
      return true;
    }
  }
  if (!mail && capsule_mode == 0 && m->message == WM_KEYDOWN &&
      (m->hwnd != C(Search) || m->wParam == VK_RETURN ||
       m->wParam == VK_UP || m->wParam == VK_DOWN || GetWindowTextLengthW(C(Search)) == 0)) {
    if ((GetKeyState(VK_CONTROL) & 0x8000) && m->wParam == 'C') {
      Act(Copy);
      return true;
    }
    if (m->wParam == VK_RETURN) {
      Act(module == Module::Passwords   ? Reveal
          : module == Module::Resources ? Asset
                                        : 238);
      return true;
    }
    if (m->wParam == VK_SPACE && module == Module::Passwords) {
      Act(Reveal);
      return true;
    }
    if (detail && (m->wParam == VK_UP || m->wParam == VK_DOWN)) {
      detail_row += (m->wParam == VK_UP ? -1 : 1);
      InvalidateRect(C(Cards), nullptr, TRUE);
      return true;
    }
    if (!detail && (m->wParam == VK_UP || m->wParam == VK_DOWN)) {
      auto selected_filter = SendMessageW(C(Filter), CB_GETCURSEL, 0, 0);
      const auto next = std::clamp(static_cast<int>(selected_filter) + (m->wParam == VK_UP ? -1 : 1),
                                  0, static_cast<int>(kinds.size()) - 1);
      SetFilter(next);
      return true;
    }
    if (!detail && !rows.empty() &&
        (m->wParam == VK_LEFT || m->wParam == VK_RIGHT)) {
      int index = static_cast<int>(SendMessageW(C(List), LB_GETCURSEL, 0, 0));
      index = std::clamp(index + (m->wParam == VK_LEFT ? -1 : 1), 0,
                         static_cast<int>(rows.size()) - 1);
      Select(index);
      SendMessageW(C(List), LB_SETCURSEL, index, 0);
      RECT r{};
      GetClientRect(C(Cards), &r);
      const int count = (std::max)(1, MulDiv(r.right, 96, dpi) / 214);
      if (index < card_offset) card_offset = index;
      if (index >= card_offset + count) card_offset = index - count + 1;
      InvalidateRect(C(Cards), nullptr, TRUE);
      return true;
    }
  }
  // The drawer is modal to keyboard focus as well as pointer input.
  if ((drawer || inspector_drawer) && mail && m->message == WM_KEYDOWN &&
      m->wParam == VK_TAB) {
    std::vector<HWND> stops;
    const std::vector<int> candidates =
        inspector_drawer ? std::vector<int>{CloseInspector,   InspectorTab,
                                            InspectorTab + 1, InspectorTab + 2,
                                            InspectorList,    InspectorBody,
                                            Receive}
                         : std::vector<int>{CloseList,        New,
                                            Search,           FilterButton,
                                            FilterButton + 1, FilterButton + 2,
                                            FilterButton + 3, List,
                                            Import,           Paste};
    for (int id : candidates)
      if (IsWindowVisible(C(id)) && IsWindowEnabled(C(id)))
        stops.push_back(C(id));
    auto at = std::find(stops.begin(), stops.end(), GetFocus());
    int index = at == stops.end()
                    ? 0
                    : static_cast<int>(at - stops.begin()) +
                          ((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
    if (!stops.empty())
      SetFocus(stops[(index + static_cast<int>(stops.size())) % stops.size()]);
    return true;
  }
  if (module == Module::Terminal && m->hwnd == C(Body)) {
    auto* t = CurrentTerminal();
    if (!t) return false;
    if (m->message == WM_CHAR) {
      const wchar_t c = static_cast<wchar_t>(m->wParam);
      if (c >= 0xd800 && c <= 0xdbff) {
        terminal_surrogate = c;
        return true;
      }
      std::wstring input;
      if (terminal_surrogate) {
        input += terminal_surrogate;
        terminal_surrogate = 0;
      }
      input += c;
      t->process->Input(Utf8(input));
      return true;
    }
    if (m->message == WM_KEYDOWN) {
      const char* key = nullptr;
      switch (m->wParam) {
        case VK_UP:
          key = "\x1b[A";
          break;
        case VK_DOWN:
          key = "\x1b[B";
          break;
        case VK_RIGHT:
          key = "\x1b[C";
          break;
        case VK_LEFT:
          key = "\x1b[D";
          break;
        case VK_HOME:
          key = "\x1b[H";
          break;
        case VK_END:
          key = "\x1b[F";
          break;
        case VK_DELETE:
          key = "\x1b[3~";
          break;
        default:
          break;
      }
      if (key) {
        t->process->Input(key);
        return true;
      }
      if (m->wParam == VK_TAB) {
        t->process->Input("\t");
        return true;
      }
      if (m->wParam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        t->process->Input(ClipboardText(hwnd));
        return true;
      }
      return false;  // TranslateMessage must create character/Enter events.
    }
  }
  if (m->message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) &&
      m->wParam == 'S' && !mail) {
    Act(Save);
    return true;
  }
  if (m->message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) &&
      m->wParam == VK_RETURN && mail) {
    Act(Send);
    return true;
  }
  return IsDialogMessageW(hwnd, m) != FALSE;
}
LRESULT CALLBACK Windows::Impl::Pane::EditProc(HWND hwnd, UINT message,
                                               WPARAM wp, LPARAM lp, UINT_PTR,
                                               DWORD_PTR data) {
  auto* self = reinterpret_cast<Pane*>(data);
  if (self->module == Module::Passwords &&
      (message == WM_COPY || message == WM_CUT)) {
    try {
      self->Act(Copy);
    } catch (const std::exception& e) {
      self->StatusText(e.what());
    }
    return 0;
  }
  if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, EditProc, 1);
  return DefSubclassProc(hwnd, message, wp, lp);
}
LRESULT CALLBACK Windows::Impl::Pane::ControlProc(HWND child, UINT message,
                                                  WPARAM wp, LPARAM lp,
                                                  UINT_PTR, DWORD_PTR data) {
  auto* self = reinterpret_cast<Pane*>(data);
  const int id = GetDlgCtrlID(child);
  try {
    const bool rail =
        !self->mail && self->capsule_mode == 0 && !self->standalone_search;
    if (id == Cards && message == WM_ERASEBKGND) return 1;
    if (rail && message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (rail && id == Search && message == WM_LBUTTONDOWN) {
      if (self->borrowed) {
        self->search_all = true;
        InvalidateRect(child, nullptr, TRUE);
      } else {
        self->standalone_search = true;
        self->ApplyWindowRole();
        SetForegroundWindow(self->hwnd);
        SetFocus(child);
      }
      return 0;
    }
    if (rail && id != Cards && id >= New &&
        (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)) {
      if (message == WM_LBUTTONDOWN) {
        SetCapture(child);
        SendMessageW(child, BM_SETSTATE, TRUE, 0);
      } else if (GetCapture() == child) {
        ReleaseCapture();
        SendMessageW(child, BM_SETSTATE, FALSE, 0);
        RECT rect{};
        GetClientRect(child, &rect);
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (PtInRect(&rect, point)) self->Act(id);
      }
      return 0;
    }
    if (self->borrowed && id == Search &&
        (message == WM_PAINT || message == WM_PRINTCLIENT ||
         message == WM_PRINT)) {
      PAINTSTRUCT ps{};
      auto dc = message == WM_PAINT ? BeginPaint(child, &ps)
                                    : reinterpret_cast<HDC>(wp);
      appearance::Paint paint(dc, self->dpi);
      auto c = appearance::Palette(self->theme, false);
      RECT r{};
      GetClientRect(child, &r);
      paint.Fill(r, self->search_all ? c.selection : c.field);
      auto query = Wide(self->Get(Search) + self->borrowed_preedit);
      paint.Text(query.empty() ? L"输入搜索…" : query + L"│", r, 12,
                 query.empty() ? c.secondary : c.text);
      if (message == WM_PAINT) EndPaint(child, &ps);
      return 0;
    }
    if (message == WM_IME_STARTCOMPOSITION) self->composing = true;
    if (message == WM_IME_ENDCOMPOSITION) self->composing = false;
    if (message == WM_KILLFOCUS) self->composing = false;
    if ((id == Route || id == Kind) &&
        (message == WM_PAINT || message == WM_PRINTCLIENT ||
         message == WM_PRINT)) {
      PAINTSTRUCT ps{};
      auto dc = message == WM_PAINT ? BeginPaint(child, &ps)
                                    : reinterpret_cast<HDC>(wp);
      appearance::Paint paint(dc, self->dpi);
      const auto c = appearance::Palette(self->theme, self->mail);
      RECT r{};
      GetClientRect(child, &r);
      paint.Fill(r, c.field, 5);
      paint.Stroke(r, GetFocus() == child ? c.accent : c.border, 5);
      RECT text = r;
      text.left += paint.S(7);
      text.right -= paint.S(22);
      std::wstring value;
      const auto index = SendMessageW(child, CB_GETCURSEL, 0, 0);
      if (index >= 0) {
        const auto length = SendMessageW(child, CB_GETLBTEXTLEN, index, 0);
        if (length >= 0 && length < 8192) {
          value.resize(static_cast<std::size_t>(length) + 1);
          SendMessageW(child, CB_GETLBTEXT, index,
                       reinterpret_cast<LPARAM>(value.data()));
          value.resize(static_cast<std::size_t>(length));
        }
      }
      paint.Text(value, text, 11,
                 IsWindowEnabled(child) ? c.text : c.secondary);
      auto arrow = r;
      arrow.left = r.right - paint.S(18);
      arrow.right -= paint.S(5);
      arrow.top += paint.S(4);
      arrow.bottom -= paint.S(4);
      ui::DrawFluentIcon(dc, ui::FluentIcon::kChevronDown, arrow, Color(c.secondary));
      if (message == WM_PAINT) EndPaint(child, &ps);
      return 0;
    }
    if ((id == Compose || id == Search || id == Title || id == Summary) &&
        (message == WM_PAINT || message == WM_PRINTCLIENT ||
         message == WM_PRINT) &&
        !GetWindowTextLengthW(child) && !self->composing) {
      const auto result = DefSubclassProc(child, message, wp, lp);
      auto dc = message == WM_PAINT ? GetDC(child) : reinterpret_cast<HDC>(wp);
      appearance::Paint paint(dc, self->dpi);
      const auto c = appearance::Palette(self->theme, self->mail);
      RECT r{};
      GetClientRect(child, &r);
      r.left += paint.S(2);
      auto hint =
          id == Compose  ? (self->module == Module::Inbox      ? L"写下本地备注…"
                            : self->module == Module::Terminal ? L"输入命令…"
                                                               : L"输入消息…")
          : id == Search ? (self->mail ? L"搜索对话或收件" : L"搜索标题或内容")
          : id == Title  ? L"标题"
                         : L"摘要（可选）";
      if (id == Compose) {
        r.top += paint.S(6);
        r.bottom = r.top + paint.S(22);
      }
      paint.Text(hint, r, 12, c.secondary);
      if (message == WM_PAINT) ReleaseDC(child, dc);
      return result;
    }
    if (message == WM_SETCURSOR &&
        ((id >= New && id < FilterButton) || id >= FilterButton)) {
      if (id != Heading && id != Subtitle && id != Preview) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
    }
    if (message == WM_MOUSEMOVE) {
      if (self->hover_control != child) {
        if (self->hover_control == self->C(Cards))
          self->HoverCard(-1);
        else if (self->hover_control)
          InvalidateRect(self->hover_control, nullptr, FALSE);
        self->hover_control = child;
        if (id != Cards) InvalidateRect(child, nullptr, FALSE);
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, child, 0};
        TrackMouseEvent(&track);
      }
      if (id == Cards) {
        const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const int x = MulDiv(point.x, 96, self->dpi);
        constexpr int pitch = appearance::kCardWidth + appearance::kCardGap;
        const int index = self->card_offset + x / pitch;
        RECT client{};
        GetClientRect(child, &client);
        self->HoverCard(!self->detail && PtInRect(&client, point) &&
                                x % pitch < appearance::kCardWidth &&
                                index < static_cast<int>(self->rows.size())
                            ? index : -1);
      }
    }
    if (message == WM_MOUSELEAVE && self->hover_control == child) {
      self->hover_control = nullptr;
      if (id == Cards) self->HoverCard(-1);
      else InvalidateRect(child, nullptr, FALSE);
    }
    if (message == WM_CAPTURECHANGED && id == Cards) {
      self->pressed_card = -1;
      self->pressed_action = 0;
    }
    if (message == WM_LBUTTONDOWN && id == Cards) {
      if (!rail) SetFocus(child);
      const int x = MulDiv(GET_X_LPARAM(lp), 96, self->dpi),
                y = MulDiv(GET_Y_LPARAM(lp), 96, self->dpi);
      if (self->detail) {
        if (x < 206 && y < 30)
          self->Act(Back);
        else if (x >= 214) {
          self->detail_row = (std::max)(0, self->detail_row - 3) +
                             (std::max)(0, (y - 10) / 26);
          InvalidateRect(child, nullptr, FALSE);
        }
        return 0;
      }
      const int index = self->card_offset + x / 214;
      if (x % 214 < 206 && index >= 0 &&
          index < static_cast<int>(self->rows.size())) {
        const bool chosen =
            self->rows[index].value("id", "") == self->selected.value("id", "");
        if (!chosen) self->Select(index);
        SendMessageW(self->C(List), LB_SETCURSEL, index, 0);
        self->pressed_card = index;
        self->pressed_action = x % 214 >= 176 ? (y < 32 && chosen ? More
                                                 : y >= 98        ? Copy
                                                                  : 0)
                                              : 0;
        SetCapture(child);
        InvalidateRect(child, nullptr, TRUE);
      }
      return 0;
    }
    if (message == WM_LBUTTONUP && id == Cards) {
      const int x = MulDiv(GET_X_LPARAM(lp), 96, self->dpi),
                y = MulDiv(GET_Y_LPARAM(lp), 96, self->dpi);
      const int action = self->pressed_action, index = self->pressed_card;
      const bool owned = GetCapture() == child;
      ReleaseCapture();
      self->pressed_card = -1;
      self->pressed_action = 0;
      if (owned && x >= 0 && y >= 0 && y < 126 &&
          index == self->card_offset + x / 214 && x % 214 >= 176 &&
          x % 214 < 206) {
        if (action == More && y < 32)
          self->ItemMenu();
        else if (action == Copy && y >= 98)
          self->Act(Copy);
      }
      return 0;
    }
    if (message == WM_LBUTTONDBLCLK && id == Cards) {
      const int x = MulDiv(GET_X_LPARAM(lp), 96, self->dpi),
                y = MulDiv(GET_Y_LPARAM(lp), 96, self->dpi);
      const int index = self->card_offset + x / 214;
      if (!self->detail && (x < 0 || y < 0 || y >= 126 || x % 214 >= 206 ||
          index < 0 || index >= static_cast<int>(self->rows.size()) ||
          (x % 214 >= 176 && (y < 32 || y >= 98)))) return 0;
      if (!self->detail && self->rows[index].value("id", "") != self->selected.value("id", ""))
        self->Select(index);
      if (!self->selected.empty()) {
        if (self->module == Module::Temporary) {
          self->Act(238);
        } else if (self->module == Module::Notes ||
            self->module == Module::Passwords) {
          self->detail = true;
          self->detail_row = 0;
          InvalidateRect(child, nullptr, TRUE);
          if (self->module == Module::Passwords && !self->gate.Authorized())
            self->Act(Reveal);
        } else
          self->Act(Edit);
      }
      return 0;
    }
    if ((message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) &&
        id == Cards) {
      if (self->detail)
        self->detail_row += GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1;
      else
        self->card_offset = std::clamp(
            self->card_offset + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1), 0,
            (std::max)(0, static_cast<int>(self->rows.size()) - 1));
      self->hover_card = -1;
      self->reveal_until = 0;
      InvalidateRect(child, nullptr, TRUE);
      return 0;
    }
    if (message == WM_CONTEXTMENU && (id == Cards || id == List)) {
      self->ItemMenu();
      return 0;
    }
    if (message == WM_LBUTTONUP && id == Scrim) {
      self->Act(Scrim);
      return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(child, ControlProc, 2);
  } catch (const std::exception& e) {
    self->StatusText(e.what());
  }
  return DefSubclassProc(child, message, wp, lp);
}
LRESULT CALLBACK Windows::Impl::Pane::Proc(HWND hwnd, UINT message, WPARAM wp,
                                           LPARAM lp) {
  auto* self = reinterpret_cast<Pane*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    self = static_cast<Pane*>(
        reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    self->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  if (!self) return DefWindowProcW(hwnd, message, wp, lp);
  try {
    switch (message) {
      case OutsideClick::kDismiss:
        if (self->outside_click.Current(wp) && !self->mail && self->capsule_mode == 0) {
          self->host.pending_paste.reset();
          self->host.runtime.ReleasePeripheral();
          self->borrowed = false;
          self->Lock();
          ShowWindow(hwnd, SW_HIDE);
        }
        return 0;
      case WM_SHOWWINDOW:
        if (!wp) self->outside_click.Stop();
        break;
      case WM_INPUT: {
        if ((self->unlocking || self->passcode_change) &&
            GetForegroundWindow() == hwnd) {
          RAWINPUT input{};
          UINT size = sizeof(input);
          const auto count =
              GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT,
                              &input, &size, sizeof(RAWINPUTHEADER));
          if (count != static_cast<UINT>(-1) &&
              count >= sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD) &&
              input.header.dwType == RIM_TYPEKEYBOARD)
            self->Challenge(input.data.keyboard.VKey,
                            (input.data.keyboard.Flags & RI_KEY_BREAK) == 0);
        }
        break;  // DefWindowProc releases the foreground raw-input allocation.
      }
      case WM_SIZE:
        if (!self->controls.empty()) self->Layout();
        return 0;
      case WM_GETMINMAXINFO: {
        auto* p = reinterpret_cast<MINMAXINFO*>(lp);
        int w = self->mail ? 680 : self->capsule_mode == 2 ? 360 : 620;
        int h = self->mail                ? 500
                : self->capsule_mode == 2 ? 300
                : self->capsule_mode == 0 ? 208
                                          : 430;
        RECT r{0, 0, MulDiv(w, self->dpi, 96), MulDiv(h, self->dpi, 96)};
        AdjustWindowRectExForDpi(
            &r, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
            static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)),
            self->dpi);
        p->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
        if (!self->mail && self->capsule_mode == 0)
          p->ptMaxTrackSize.y = r.bottom - r.top;
        return 0;
      }
      case WM_NCHITTEST: {
        const auto hit = DefWindowProcW(hwnd, message, wp, lp);
        if (hit != HTCLIENT || self->mail) return hit;
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        if (pt.y < MulDiv(46, self->dpi, 96) &&
            ChildWindowFromPointEx(hwnd, pt, CWP_SKIPINVISIBLE) == hwnd)
          return HTCAPTION;
        return HTCLIENT;
      }
      case WM_MOUSEACTIVATE:
        if (!self->mail && self->capsule_mode == 0 && !self->standalone_search)
          return MA_NOACTIVATE;
        break;
      case WM_DPICHANGED: {
        auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left,
                     r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        self->Theme();
        self->Layout();
        return 0;
      }
      case WM_SETTINGCHANGE:
        self->Theme();
        return 0;
      case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
          self->Lock();
          // Native activation can briefly have no destination while a first
          // internal click is being routed. A null HWND is not an outside hit;
          // the physical mouse hook still handles actual outside clicks.
          if (lp && !self->mail && self->capsule_mode == 0 && self->standalone_search)
            self->outside_click.Observe(reinterpret_cast<HWND>(lp));
        }
        return 0;
      case WM_CLIPBOARDUPDATE:
        self->host.ClipboardChanged();
        return 0;
      case WM_COMMAND: {
        const auto id = LOWORD(wp), notification = HIWORD(wp);
        if (self->loading) return 0;
        if (id == InspectorList && notification == LBN_SELCHANGE) {
          self->SelectInspector(static_cast<int>(
              SendMessageW(self->C(InspectorList), LB_GETCURSEL, 0, 0)));
          return 0;
        }
        if (id == Route && notification == CBN_SELCHANGE &&
            self->module == Module::Chat && !self->selected.empty() &&
            self->selected.value("messages", Json::array()).empty()) {
          auto settings = self->host.runtime.Configuration();
          const auto choice = SendMessageW(self->C(Route), CB_GETCURSEL, 0, 0);
          if (choice >= 0 &&
              static_cast<std::size_t>(choice) < settings.connections.size()) {
            workbench::SelectConnection(&settings,
                                        settings.connections[choice].id);
            self->selected["route"] = FreezeRoute(settings);
            self->selected = self->host.store.Save(
                self->selected, self->selected.at("revision"));
          }
          return 0;
        }
        if (id == List && notification == LBN_SELCHANGE)
          self->Select(static_cast<int>(
              SendMessageW(self->C(List), LB_GETCURSEL, 0, 0)));
        else if (id == List && notification == LBN_DBLCLK && !self->mail)
          self->Act(Edit);
        else if (id == Search && notification == EN_CHANGE)
          self->Refresh();
        else if (id == Filter && notification == CBN_SELCHANGE) {
          const auto i = SendMessageW(self->C(Filter), CB_GETCURSEL, 0, 0);
          if (i >= 0 && static_cast<std::size_t>(i) < self->kinds.size()) {
            self->SetFilter(static_cast<std::size_t>(i));
          }
        } else if ((id == Title || id == Summary || id == Body) &&
                   notification == EN_CHANGE)
          self->dirty = true;
        else if (id >= New && notification == BN_CLICKED)
          self->Act(id);
        return 0;
      }
      case WM_MEASUREITEM:
        if (wp == List) {
          reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight =
              MulDiv(self->mail ? 84 : 57, self->dpi, 96);
          return TRUE;
        }
        break;
      case WM_DRAWITEM: {
        auto* d = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        const auto c = appearance::Palette(self->theme, self->mail);
        appearance::Paint p(d->hDC, self->dpi);
        if (d->CtlID == List) {
          self->DrawList(d);
          return TRUE;
        }
        if (d->CtlID == Cards) {
          self->DrawCards(d);
          return TRUE;
        }
        if (d->CtlID == Heading || d->CtlID == Subtitle) {
          p.Fill(d->rcItem, c.background);
          p.Text(Text(d->hwndItem), d->rcItem, d->CtlID == Heading ? 16 : 11,
                 d->CtlID == Heading ? c.text : c.secondary,
                 d->CtlID == Heading ? FW_SEMIBOLD : FW_NORMAL);
          return TRUE;
        }
        if (d->CtlID == Scrim) {
          const int saved = SaveDC(d->hDC);
          OffsetViewportOrgEx(d->hDC, -p.S(64), 0, nullptr);
          self->Paint(d->hDC);
          RestoreDC(d->hDC, saved);
          for (int id : std::initializer_list<int>{
                   Heading, Subtitle, Title, Body, Preview, Compose, Send,
                   Route, Status, Sidebar, Edit, Save, Archive, More, End,
                   Inspector, 237})
            if (IsWindowVisible(self->C(id))) {
              RECT r{};
              GetWindowRect(self->C(id), &r);
              MapWindowPoints(nullptr, self->hwnd, reinterpret_cast<POINT*>(&r),
                              2);
              const int child_saved = SaveDC(d->hDC);
              OffsetViewportOrgEx(d->hDC, r.left - p.S(64), r.top, nullptr);
              SendMessageW(self->C(id), WM_PRINT,
                           reinterpret_cast<WPARAM>(d->hDC),
                           PRF_CLIENT | PRF_ERASEBKGND);
              RestoreDC(d->hDC, child_saved);
            }
          auto overlay = CreateCompatibleDC(d->hDC);
          auto bitmap = CreateCompatibleBitmap(d->hDC, 1, 1);
          auto old = SelectObject(overlay, bitmap);
          SetPixel(overlay, 0, 0, RGB(0, 0, 0));
          BLENDFUNCTION blend{AC_SRC_OVER, 0, 40, 0};
          AlphaBlend(d->hDC, 0, 0, d->rcItem.right, d->rcItem.bottom, overlay,
                     0, 0, 1, 1, blend);
          SelectObject(overlay, old);
          DeleteObject(bitmap);
          DeleteDC(overlay);
          // Drawer background is opaque; only the remaining main surface dims.
          RECT client{};
          GetClientRect(hwnd, &client);
          const auto l = appearance::Mail(MulDiv(client.right, 96, self->dpi),
                                          self->module, self->sidebar_hidden,
                                          self->drawer);
          p.Fill(p.R(0, 0, l.list_width, MulDiv(client.bottom, 96, self->dpi)),
                 c.sidebar);
          p.Text(self->module == Module::Chat    ? L"对话"
                 : self->module == Module::Inbox ? L"收件箱"
                                                 : L"终端",
                 p.R(16, 16, l.list_width - 100, 28), 19, c.text, FW_SEMIBOLD);
          if (self->inspector_drawer) {
            const int w = client.right * 96 / static_cast<int>(self->dpi) - 64,
                      h = client.bottom * 96 / static_cast<int>(self->dpi);
            p.Fill(p.R(w - 240, 0, 240, h), c.sidebar);
            p.Text(L"扩展区", p.R(w - 224, 16, 150, 28), 13, c.text,
                   FW_SEMIBOLD);
          }
          return TRUE;
        }
        if (d->CtlID == Preview && self->thumbnail) {
          p.Fill(d->rcItem, c.background);
          BITMAP bitmap{};
          GetObjectW(self->thumbnail, sizeof(bitmap), &bitmap);
          const auto scale = (std::min)(
              static_cast<double>(d->rcItem.right) / bitmap.bmWidth,
              static_cast<double>(d->rcItem.bottom) / bitmap.bmHeight);
          const int w = static_cast<int>(bitmap.bmWidth * scale),
                    h = static_cast<int>(bitmap.bmHeight * scale);
          auto dc = CreateCompatibleDC(d->hDC);
          auto old = SelectObject(dc, self->thumbnail);
          SetStretchBltMode(d->hDC, HALFTONE);
          StretchBlt(d->hDC, (d->rcItem.right - w) / 2,
                     (d->rcItem.bottom - h) / 2, w, h, dc, 0, 0, bitmap.bmWidth,
                     bitmap.bmHeight, SRCCOPY);
          SelectObject(dc, old);
          DeleteDC(dc);
          return TRUE;
        }
        if (d->CtlType == ODT_BUTTON) {
          self->DrawButton(d);
          return TRUE;
        }
        break;
      }
      case WM_CTLCOLOREDIT:
      case WM_CTLCOLORLISTBOX:
      case WM_CTLCOLORSTATIC: {
        const auto c = appearance::Palette(self->theme, self->mail);
        const auto id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        const auto bg =
            id == Status ? c.background
            : (self->mail && (id == Compose || id == List ||
                              id == InspectorList || id == InspectorBody))
                ? c.sidebar
                : c.field;
        auto dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, Color(id == Status ? c.secondary : c.text));
        SetBkColor(dc, Color(bg));
        SetDCBrushColor(dc, Color(bg));
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
      }
      case WM_ERASEBKGND:
        return 1;
      case WM_PRINTCLIENT:
        self->Paint(reinterpret_cast<HDC>(wp));
        return 0;
      case WM_PAINT: {
        PAINTSTRUCT paint{};
        auto dc = BeginPaint(hwnd, &paint);
        RECT r{};
        GetClientRect(hwnd, &r);
        const auto frame = CreateCompatibleDC(dc);
        const auto bitmap =
            frame ? CreateCompatibleBitmap(dc, (std::max)(1L, r.right),
                                           (std::max)(1L, r.bottom))
                  : nullptr;
        if (bitmap) {
          const auto previous = SelectObject(frame, bitmap);
          self->Paint(frame);
          BitBlt(dc, 0, 0, r.right, r.bottom, frame, 0, 0, SRCCOPY);
          SelectObject(frame, previous);
          DeleteObject(bitmap);
        } else
          self->Paint(dc);
        if (frame) DeleteDC(frame);
        EndPaint(hwnd, &paint);
        return 0;
      }
      case WM_CLOSE:
        if (!self->Leave()) return 0;
        if (!self->mail) {
          self->host.runtime.ReleasePeripheral();
          self->borrowed = false;
        }
        self->Lock();
        {
          RECT r{};
          GetClientRect(hwnd, &r);
          auto prefs = self->host.store.Preferences();
          if (self->mail || self->capsule_mode == 1)
            prefs[self->mail ? "mailbox_client_size"
                             : "capsule_manager_client_size"] =
                Json::array({MulDiv(r.right - r.left, 96, self->dpi),
                             MulDiv(r.bottom - r.top, 96, self->dpi)});
          self->host.store.Preferences(prefs);
        }
        ShowWindow(hwnd, SW_HIDE);
        return 0;
      case WM_DESTROY:
        RemoveClipboardFormatListener(hwnd);
        self->hwnd = nullptr;
        return 0;
    }
  } catch (const std::exception& e) {
    self->StatusText(e.what());
  }
  return DefWindowProcW(hwnd, message, wp, lp);
}
void Windows::Impl::Open(Module m, int mode) {
  if (protected_session) return;
  pending_paste.reset();
  feedback.Hide();
  if (static_cast<unsigned>(m) >= 5) runtime.PauseCapture();
  auto& pane = static_cast<unsigned>(m) >= 5 ? mailbox : capsule;
  if (!pane)
    pane = std::make_unique<Pane>(*this, static_cast<unsigned>(m) >= 5);
  if (!pane->mail) {
    pane->opening_foreground = GetForegroundWindow();
    pane->opening_paste_target = actions.target();
  }
  pane->Open(m, mode);
  if (!pane->mail && mode == 0) pane->Borrow();
}
void Windows::Impl::Pane::Borrow() {
  const auto process = host.actions.target().process;
  borrowed = host.runtime.BorrowPeripheral(process);
  borrowed_preedit.clear();
  if (!borrowed) {
    // Detached invocation must still be ready for search/navigation on its
    // first keystroke, without first requiring a click on the search field.
    standalone_search = true;
    ApplyWindowRole();
    SetForegroundWindow(hwnd);
    SetFocus(C(Search));
  }
  if (!outside_click.Start(hwnd) && borrowed) {
    host.runtime.ReleasePeripheral();
    borrowed = false;
    standalone_search = true;
    ApplyWindowRole();
    SetForegroundWindow(hwnd);
    SetFocus(C(Search));
  }
  StatusText(borrowed ? "直接输入搜索 · ← → 选择 · Enter 插入 · Esc 返回"
                      : "直接输入搜索 · ← → 选择 · Enter 使用 · Esc 取消");
}
void Windows::Impl::Pane::CardImages() {
  if (card_image_job.valid()) {
    if (card_image_job.wait_for(std::chrono::seconds(0)) !=
        std::future_status::ready)
      return;
    auto [id, bitmap] = card_image_job.get();
    card_images[id] = bitmap;
    InvalidateRect(C(Cards), nullptr, TRUE);
  }
  if (!hwnd || !IsWindowVisible(hwnd) || capsule_mode != 0) return;
  if (card_images.size() > 32) {
    for (auto [id, bitmap] : card_images)
      if (bitmap) DeleteObject(bitmap);
    card_images.clear();
  }
  for (int i = card_offset;
       i < (std::min)(card_offset + 6, static_cast<int>(rows.size())); ++i) {
    const auto& row = rows[i];
    const auto id = row.value("asset", "");
    if (row.value("kind", "") != "image" || id.empty() ||
        card_images.contains(id))
      continue;
    auto path = host.store.Asset(row);
    card_image_job = std::async(std::launch::async, [id, path] {
      const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      auto bitmap = Thumbnail(path, 400);
      if (SUCCEEDED(com)) CoUninitialize();
      return std::make_pair(id, bitmap);
    });
    break;
  }
}
void Windows::Impl::Pane::BorrowedInput() {
  if (!borrowed) return;
  auto input = host.runtime.TakePeripheralInput();
  DWORD foreground = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
  if (!IsWindowVisible(hwnd) || capsule_mode != 0 ||
      !input.value("active", false) ||
      foreground != input.value("process", 0U)) {
    host.runtime.ReleasePeripheral();
    borrowed = false;
    borrowed_preedit.clear();
    Lock();
    ShowWindow(hwnd, SW_HIDE);
    return;
  }
  borrowed_preedit = input.value("preedit", "");
  for (const auto& e : input["events"]) {
    if (e.contains("delivered")) {
      if (e.value("delivered", false)) {
        if (module == Module::Temporary && !selected.empty())
          selected = host.store.PromoteTemporary(selected);
        host.runtime.ReleasePeripheral();
        borrowed = false;
        Lock();
        ShowWindow(hwnd, SW_HIDE);
        return;
      }
      StatusText("目标输入框未接受内容；条目已保留。");
    } else if (e.contains("text")) {
      auto query = search_all ? std::wstring{} : Wide(Get(Search));
      query += Wide(e.value("text", ""));
      if (query.size() <= 1024) Set(Search, Utf8(query));
      search_all = false;
      detail = false;
    } else {
      const auto key = e.value("key", 0U);
      const bool down = e.value("down", false);
      if (e.value("physical", false)) {
        if (key == VK_ESCAPE && down) {
          Lock();
          StatusText("已取消验证。");
        } else
          Challenge(key, down);
        continue;
      }
      if (!down) continue;
      const bool ctrl =
          (e.value("modifiers", 0U) &
           static_cast<unsigned>(core::KeyModifiers::kControl)) != 0;
      if (ctrl && key == 'A')
        search_all = true;
      else if (ctrl && key == 'C')
        Act(Copy);
      else if (ctrl && key == 'V') {
        auto query = search_all ? std::string{} : Get(Search);
        auto text = ClipboardText(hwnd);
        if (query.size() + text.size() <= 4096) Set(Search, query + text);
        search_all = false;
      } else if (ctrl && key == 'N')
        Act(New);
      else if (ctrl && key == 'F') {
        detail = false;
        search_all = true;
      } else if (key == VK_BACK || key == VK_DELETE) {
        auto query = Wide(Get(Search));
        if (search_all || key == VK_DELETE)
          query.clear();
        else if (!query.empty()) {
          const auto last = query.back();
          query.pop_back();
          if (last >= 0xdc00 && last <= 0xdfff && !query.empty() &&
              query.back() >= 0xd800 && query.back() <= 0xdbff)
            query.pop_back();
        }
        search_all = false;
        Set(Search, Utf8(query));
      } else if (key == VK_TAB) {
        auto i = std::find_if(kinds.begin(), kinds.end(),
                              [&](const auto& k) { return filter == k.first; });
        SetFilter((static_cast<std::size_t>(i - kinds.begin()) + 1) %
                  kinds.size());
      } else {
        MSG msg{};
        msg.hwnd = C(Cards);
        msg.message = WM_KEYDOWN;
        msg.wParam = key;
        Key(&msg);
      }
      if (!borrowed || capsule_mode != 0 || !IsWindowVisible(hwnd)) break;
    }
  }
  RECT caret{};
  GetWindowRect(C(Search), &caret);
  caret.left = caret.right - MulDiv(8, dpi, 96);
  caret.right = caret.left + 1;
  host.runtime.PublishPeripheralCaret(caret);
  InvalidateRect(C(Search), nullptr, TRUE);
}
void Windows::Impl::Manage(Pane& pane) {
  auto menu = CreatePopupMenu();
  const int start = pane.mail ? 5 : 0, end = pane.mail ? 8 : 5;
  for (int i = start; i < end; ++i)
    AppendMenuW(
        menu,
        MF_STRING | (store.Enabled(static_cast<Module>(i)) ? MF_CHECKED : 0),
        i + 1, kModuleNames[i]);
  POINT p{};
  GetCursorPos(&p);
  const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, p.x,
                                      p.y, 0, pane.hwnd, nullptr);
  DestroyMenu(menu);
  if (command) {
    const auto m = static_cast<Module>(command - 1);
    SetEnabled(m, !store.Enabled(m));
  }
}
void Windows::Impl::SetEnabled(Module module, bool enabled) {
  store.Enable(module, enabled);
  if (!enabled && module == Module::Capture && capture) capture->Protect();
  // Settings and each host's module menu share one enablement authority.
  // Revocation is synchronous, including borrowed input and streaming jobs.
  if (!enabled && module == Module::Chat) cancel.store(true);
  for (auto* pane : {capsule.get(), mailbox.get()}) {
    if (!pane || !pane->hwnd) continue;
    if (!enabled && pane->module == module) {
      pane->Lock();
      if (pane->borrowed) runtime.ReleasePeripheral();
      pane->borrowed = false;
      ShowWindow(pane->hwnd, SW_HIDE);
    } else {
      pane->Layout();
    }
  }
}
bool Windows::Impl::Authorized() {
  const auto plugins = runtime.Plugins();
  std::string current, route;
  for (const auto& p : plugins)
    if (p.enabled && p.installed && p.id == connector) {
      current = p.grant;
      route = p.grant;
    }
  return !grant.empty() && !connector_grant.empty() && grant == current &&
         connector_grant == route && store.Enabled(Module::Chat);
}
void Windows::Impl::Generate(Pane& pane) {
  {
    std::lock_guard lock(job_mutex);
    if (!job.id.empty())
      throw std::runtime_error("已有对话正在生成，请等待或停止。");
  }
  auto message = pane.Get(Compose);
  if (message.empty()) return;
  if (pane.selected.empty()) {
    pane.Act(New);
  }
  auto record = pane.selected;
  auto config = ResolveRoute(record.at("route"), runtime.Configuration());
  connector = config.ai_connector == "codex-cli" ? "builtin.codex-cli"
                                                 : "builtin.openai-compatible";
  grant.clear();
  connector_grant.clear();
  for (const auto& p : runtime.Plugins())
    if (p.installed && p.enabled && p.id == connector) {
      grant = p.grant;
      connector_grant = p.grant;
    }
  if (!Authorized())
    throw std::runtime_error("请在设置中启用 AI 生成及此对话的连接器。");
  record["messages"].push_back({{"id", NewId()},
                                {"time", MessageTime()},
                                {"role", "user"},
                                {"body", message}});
  record["draft"] = "";
  workbench::Generation generation{};
  generation.source = ConversationPrompt(record);
  generation.instruction =
      "Continue this conversation. The input is a JSON array of prior "
      "role/body messages. Reply to the final user message in their language. "
      "Do not include role prefixes.";
  record = store.Save(record, record.at("revision"));
  pane.selected = record;
  pane.Load();
  if (worker.joinable()) worker.join();
  cancel.store(false);
  {
    std::lock_guard lock(job_mutex);
    job = {record.at("id"), "", "", record, false, false};
  }
  pane.generating = true;
  SetWindowTextW(pane.C(Send), L"停止");
  worker = std::jthread([this, config, generation] {
    std::string error;
    bool ok = false;
    try {
      ok = generate(
          config, generation,
          [this](const std::string& text) {
            std::lock_guard lock(job_mutex);
            if (cancel.load() || job.preview.size() + text.size() > 256 * 1024)
              return false;
            job.preview += text;
            return true;
          },
          [this] { return cancel.load(); }, &error);
    } catch (...) {
      error = "生成中断，请重试。";
    }
    std::lock_guard lock(job_mutex);
    job.success = ok && !cancel.load();
    job.error = error;
    job.done = true;
  });
}
void Windows::Impl::Tick() {
  if (!protected_session && GetTickCount64() >= cleanup_due) Cleanup();
  if (pending_paste && actions.now() >= pending_paste->earliest) {
    const auto pending = *pending_paste;
    const auto result = !runtime.CanPastePeripheral(pending.ticket) ? PasteResult::TargetChanged :
        actions.now() >= pending.deadline ? PasteResult::Waiting :
        actions.paste(pending.target, pending.sequence);
    if (result != PasteResult::Waiting || actions.now() >= pending.deadline) {
      pending_paste.reset();
      feedback.Show(result == PasteResult::Sent ? L"已发送粘贴" :
          result == PasteResult::ClipboardChanged ? L"剪贴板已变化 · 未粘贴" :
          L"已复制 · 请在目标输入框粘贴", CurrentTheme(), capsule ? capsule->hwnd : nullptr);
    }
  }
  for (auto* pane : {capsule.get(), mailbox.get()})
    if (pane && pane->hwnd && pane->theme != CurrentTheme()) pane->Theme();
  if (clipboard_retry && GetTickCount64() >= clipboard_retry)
    ClipboardChanged();
  const auto revision = workbench::GatewayRevision();
  if (revision != gateway_revision) {
    gateway_revision = revision;
    if (mailbox && mailbox->hwnd) mailbox->Refresh();
  }
  if (capsule) {
    capsule->BorrowedInput();
    capsule->CardImages();
  }
  if (secret_until && GetTickCount64() >= secret_until) {
    ClearSecretClipboard();
  }
  for (auto* p : {capsule.get(), mailbox.get()})
    if (p && p->hwnd) {
      if (!store.Enabled(p->module)) {
        p->Lock();
        ShowWindow(p->hwnd, SW_HIDE);
      }
      if (p->passcode_change && !p->gate.Authorized()) {
        p->Lock();
        p->StatusText("查看授权已到期，请重新验证。");
      }
      if (p->module == Module::Passwords &&
          (!p->gate.Authorized() && !p->unlocking && !p->passcode_change) &&
          p->selected.contains("revision")) {
        if (!p->Get(Body).empty()) p->Lock();
      }
      if (p->reveal_until && GetTickCount64() >= p->reveal_until) {
        p->reveal_until = 0;
        p->loading = true;
        p->Set(Body, "");
        p->loading = false;
        p->dirty = false;
        SendMessageW(p->C(Body), EM_SETREADONLY, TRUE, 0);
      }
      if (IsWindowVisible(p->hwnd) && p->module == Module::Terminal)
        if (auto* t = p->CurrentTerminal()) {
          auto text = t->process->Output();
          if (text != p->last_terminal) {
            p->last_terminal = text;
            p->loading = true;
            SetWindowTextW(p->C(Body), text.c_str());
            p->loading = false;
          }
        }
    }
  std::lock_guard lock(job_mutex);
  if (job.id.empty()) return;
  if (!Authorized()) cancel.store(true);
  if (mailbox && mailbox->module == Module::Chat &&
      mailbox->selected.value("id", "") == job.id) {
    mailbox->StatusText(job.done ? "" : "正在生成…");
    mailbox->generating = !job.done;
    SetWindowTextW(mailbox->C(Send), job.done ? L"发送" : L"停止");
    if (mailbox->rendered_preview != job.preview) {
      mailbox->rendered_preview = job.preview;
      mailbox->RenderTranscript(job.preview);
    }
  }
  if (!job.done) return;
  try {
    if (job.success && !cancel.load() && Authorized()) {
      auto record = store.Get(job.id);
      record["messages"].push_back({{"id", NewId()},
                                    {"time", MessageTime()},
                                    {"role", "assistant"},
                                    {"format", "markdown"},
                                    {"body", job.preview}});
      store.Save(record, record.at("revision"));
    }
    if (mailbox) {
      mailbox->StoreDraft();
      mailbox->generating = false;
      mailbox->Refresh();
      mailbox->Load();
      if (mailbox->module == Module::Chat) {
        mailbox->generating = false;
        SetWindowTextW(mailbox->C(Send), L"发送");
      }
      mailbox->StatusText(job.success && !cancel.load() ? "回复已保存。"
                          : cancel.load() ? "已停止；未保存未完成的回复。"
                                          : job.error);
    }
  } catch (const std::exception& e) {
    if (mailbox) mailbox->StatusText(e.what());
  }
  Wipe(job.preview);
  job = Job{};
}
void Windows::Impl::ClipboardChanged() {
  clipboard_retry = 0;
  const auto sequence = clipboard.sequence();
  if (sequence == clipboard_sequence) return;
  clipboard_record.clear();
  if (protected_session || !clipboard.interactive() || !store.CollectClipboard() ||
      !store.Enabled(Module::Temporary))
    return;
  // The reader checks sensitive/exclusion formats while the clipboard is open.
  auto content = clipboard.read(capsule ? capsule->hwnd : nullptr);
  if (!content.opened) {
    if (++clipboard_attempts <= 10)
      clipboard_retry = GetTickCount64() + 100;
    else {
      clipboard_sequence = sequence;
      clipboard_attempts = 0;
    }
    return;
  }
  clipboard_attempts = 0;
  clipboard_sequence = sequence;
  // A damaged library must not grow without a verifiable cleanup boundary.
  Cleanup();
  if (content.formats.empty() && content.bytes.empty() && content.files.empty() && content.text.empty()) return;
  store.RecordClipboard(content);
  Cleanup();
  if (capsule && capsule->module == Module::Temporary) capsule->Refresh();
}
void Windows::Impl::Cleanup() {
  // Startup, hourly maintenance, and after collection. No host-wide mutex or
  // engine lock is held while scanning the private store.
  cleanup_due = GetTickCount64() + 3600000;
  auto protected_ids = capture ? capture->ActiveRecords() : std::set<std::string>{};
  if (!clipboard_record.empty() && clipboard.sequence() == clipboard_sequence)
    protected_ids.insert(clipboard_record);
  for (auto* pane : {capsule.get(), mailbox.get()})
    if (pane && IsWindowVisible(pane->hwnd) && pane->selected.contains("id"))
      protected_ids.insert(pane->selected.at("id"));
  const auto result = store.Cleanup(protected_ids);
  if (result.records) for (auto* pane : {capsule.get(), mailbox.get()})
    if (pane && pane->hwnd && !pane->dirty) pane->Refresh();
}
void Windows::Impl::ClearSecretClipboard() {
  if (secret_until && GetClipboardSequenceNumber() == secret_sequence &&
      OpenClipboard(nullptr)) {
    if (GetClipboardSequenceNumber() == secret_sequence) EmptyClipboard();
    CloseClipboard();
  }
  secret_until = 0;
}
void Windows::Impl::Protect() {
  protected_session = true;
  clipboard_retry = 0;
  clipboard_attempts = 0;
  pending_paste.reset();
  feedback.Hide();
  if (capture) capture->Protect();
  runtime.ReleasePeripheral();
  cancel.store(true);
  ClearSecretClipboard();
  for (auto* p : {capsule.get(), mailbox.get()})
    if (p && p->hwnd) {
      p->Lock();
      ShowWindow(p->hwnd, SW_HIDE);
    }
}
Windows::Windows(workbench::Runtime& runtime, std::filesystem::path root,
                 workbench::Runtime::APIGenerator generate, ClipboardSource clipboard,
                 ClipboardActions actions)
    : impl_(std::make_unique<Impl>(runtime, std::move(root),
                                   std::move(generate), std::move(clipboard), std::move(actions))) {}
Windows::~Windows() = default;
void Windows::Open(Module m) { impl_->Open(m); }
void Windows::Open(bool mailbox) {
  if (const auto module = mailbox ? impl_->store.MailboxSelection()
                                 : impl_->store.CapsuleSelection())
    Open(*module);
}
void Windows::Toggle(bool mailbox) {
  const auto window = Handle(mailbox);
  if (window && IsWindowVisible(window))
    SendMessageW(window, WM_CLOSE, 0, 0);
  else
    Open(mailbox);
}
void Windows::Manage(bool mailbox) {
  if (impl_->protected_session) return;
  if (const auto module = mailbox ? impl_->store.MailboxSelection()
                                 : impl_->store.CapsuleSelection()) {
    impl_->Open(*module, 1);
    if (mailbox) impl_->Manage(*impl_->mailbox);
  }
}
void Windows::Tick() {
  try {
    impl_->Tick();
  } catch (const std::exception& e) {
    if (impl_->mailbox) impl_->mailbox->StatusText(e.what());
  }
}
void Windows::Protect() { impl_->Protect(); }
void Windows::Resume() {
  if (!impl_->clipboard.interactive()) return;
  impl_->clipboard_sequence = impl_->clipboard.sequence();
  impl_->protected_session = false;
}
void Windows::PreviewTheme(std::optional<ui::ThemeId> theme) {
  impl_->preview_theme = theme;
  for (auto* pane : {impl_->capsule.get(), impl_->mailbox.get()})
    if (pane && pane->hwnd && pane->theme != impl_->CurrentTheme())
      pane->Theme();
  if (impl_->capture) impl_->capture->RefreshTheme();
}
void Windows::ClipboardChanged() { impl_->ClipboardChanged(); }
bool Windows::CollectClipboard() const { return impl_->store.CollectClipboard(); }
void Windows::SetCollectClipboard(bool enabled) {
  impl_->store.SetCollectClipboard(enabled);
  // Resuming establishes a new baseline; never import copies made while paused.
  impl_->clipboard_sequence = impl_->clipboard.sequence();
  impl_->clipboard_retry = 0;
  impl_->clipboard_attempts = 0;
}
CaptureWindows& Windows::Impl::Captures() {
  if (!capture)
    capture = std::make_unique<CaptureWindows>(
        store,
        [this] {
          cleanup_due = 0;
          if (capsule && capsule->hwnd && !capsule->dirty) {
            capsule->Refresh();
            capsule->Load();
          }
        },
        [this] { return ui::ThemeIdString(CurrentTheme()); });
  return *capture;
}
void Windows::Impl::CaptureArea() {
  if (protected_session || !InteractiveDesktop())
    throw std::runtime_error("当前桌面不可截图，请解锁后重试。");
  if (!store.Enabled(Module::Capture))
    throw std::runtime_error("捕获模块已停用，请先在插件中启用。");
  if (capsule && !capsule->Leave()) return;
  runtime.ReleasePeripheral();
  runtime.PauseCapture();
  const bool visible =
      capsule && capsule->hwnd && IsWindowVisible(capsule->hwnd);
  if (visible) ShowWindow(capsule->hwnd, SW_HIDE);
  try {
    Captures().Start();
  } catch (...) {
    if (visible) ShowWindow(capsule->hwnd, SW_SHOWNOACTIVATE);
    throw;
  }
}
void Windows::CaptureArea() {
  try {
    impl_->CaptureArea();
  } catch (const std::exception& e) {
    MessageBoxW(nullptr, Wide(e.what()).c_str(), L"截图",
                MB_OK | MB_ICONINFORMATION);
  }
}
bool Windows::HandleMessage(MSG* m) {
  try {
    if (impl_->capsule && (m->message == WM_LBUTTONDOWN || m->message == WM_RBUTTONDOWN ||
        m->message == WM_MBUTTONDOWN || m->message == WM_NCLBUTTONDOWN))
      impl_->capsule->outside_click.Observe(m->hwnd);
    return (impl_->capture && impl_->capture->HandleMessage(m)) ||
           (impl_->capsule && impl_->capsule->Key(m)) ||
           (impl_->mailbox && impl_->mailbox->Key(m));
  } catch (const std::exception& e) {
    auto* p = impl_->capsule && impl_->capsule->hwnd == GetForegroundWindow()
                  ? impl_->capsule.get()
                  : impl_->mailbox.get();
    if (p) p->StatusText(e.what());
    return true;
  }
}
HWND Windows::Handle(bool mailbox) const {
  auto* p = mailbox ? impl_->mailbox.get() : impl_->capsule.get();
  return p ? p->hwnd : nullptr;
}
std::vector<workbench::PluginView> Windows::Plugins() const {
  std::vector<workbench::PluginView> result;
  for (unsigned i = 0; i < std::size(kModuleIds); ++i) {
    workbench::PluginView view;
    view.kind = i < 5 ? "capsule" : "mailbox";
    view.id = "builtin." + view.kind + "." + kModuleIds[i];
    view.name = workbench::Utf8(kModuleNames[i]);
    view.installed = view.bundled = true;
    view.enabled = impl_->store.Enabled(static_cast<Module>(i));
    view.can_install = false;  // Compiled into this native host.
    result.push_back(std::move(view));
  }
  return result;
}
bool Windows::SetPluginEnabled(const std::string& id, bool enabled,
                               std::string* error) {
  try {
    const auto plugins = Plugins();
    for (unsigned i = 0; i < plugins.size(); ++i) {
      if (plugins[i].id != id) continue;
      impl_->SetEnabled(static_cast<Module>(i), enabled);
      return true;
    }
    throw std::runtime_error("未知宿主插件。");
  } catch (const std::exception& failure) {
    if (error) *error = failure.what();
    return false;
  }
}
}  // namespace rimes::windows::peripherals
