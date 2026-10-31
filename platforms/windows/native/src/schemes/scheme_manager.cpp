#include "../core/product_brand.hpp"
#include "../broker/default_paths.hpp"
#include "../engine/rime_engine.hpp"
#include "../workbench/provider.hpp"
#include "scheme_library.hpp"
#include <algorithm>
#include <commdlg.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>

namespace rimes::windows::schemes {
namespace {
namespace fs = std::filesystem;
namespace common = rimes::schemes;
using workbench::Utf8;
using workbench::Wide;
fs::path Executable() {
  wchar_t path[32768]{};
  const DWORD count = GetModuleFileNameW(nullptr, path, 32768);
  if (!count || count >= 32768)
    throw std::runtime_error("Cannot locate the running executable");
  return path;
}
std::wstring Quote(const std::wstring &word) {
  std::wstring result = L"\"";
  std::size_t slashes = 0;
  for (auto c : word) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
    slashes = 0;
    if (c == L'"')
      result += L'\\';
    result += c;
  }
  result.append(slashes * 2, L'\\');
  return result + L'"';
}
void Deploy(const fs::path &root, const std::vector<std::string> &ids) {
  std::wstring command = Quote(Executable().wstring()) + L" --scheme-deploy " +
                         Quote(root.wstring());
  for (const auto &id : ids)
    command += L" " + Quote(Wide(id));
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  const auto logs = root.parent_path().parent_path() / L"logs";
  fs::create_directories(logs);
  const auto log = logs / (root.filename().wstring() + L".log");
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE output =
      CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (output == INVALID_HANDLE_VALUE)
    throw std::runtime_error("Cannot create import diagnostics");
  struct Close {
    HANDLE handle;
    ~Close() {
      if (handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    }
  } close_output{output};
  HANDLE input =
      CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  &attributes, OPEN_EXISTING, 0, nullptr);
  Close close_input{input};
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = output;
  startup.hStdError = output;
  startup.hStdInput = input;
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(Executable().c_str(), command.data(), nullptr, nullptr,
                      TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                      &process))
    throw std::runtime_error("Cannot start the scheme compiler");
  CloseHandle(process.hThread);
  const DWORD result = WaitForSingleObject(process.hProcess, 600000);
  DWORD code = 1;
  if (result != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, INFINITE);
  } else
    GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  common::CheckDeploymentLog(log);
  if (code)
    throw std::runtime_error(
        "方案编译或加载失败；当前方案未改变。请检查方案依赖，以及运行库是否支持"
        "所需的 Lua 功能。导入日志位于 schemes/logs。");
}
int Compile(const fs::path &root, const std::vector<std::string> &ids) {
  broker::DefaultBrokerPaths paths;
  std::wstring path_error;
  if (!broker::ResolveDefaultBrokerPaths(&paths, &path_error))
    throw std::runtime_error(Utf8(path_error));
  engine::RimeEngineOptions options;
  options.dll_path = paths.dll_path;
  options.shared_data_dir = root;
  options.user_data_dir = root;
  options.log_dir = root.parent_path().parent_path() / L"logs";
  fs::create_directories(options.log_dir);
  options.verify_input_session = false;
  options.skip_maintenance = true;
  engine::RimeEngine engine;
  std::string error;
  if (!engine.Start(options, &error))
    throw std::runtime_error(error);
  // Lua modules may read build/default.yaml while initializing a schema.
  // Compile the isolated workspace configuration before loading any scheme.
  if (!engine.DeployDefaultConfiguration(&error))
    throw std::runtime_error(error);
  for (const auto &id : ids)
    if (!common::ValidID(id) ||
        !engine.DeploySchema(root / (id + ".schema.yaml"), &error))
      throw std::runtime_error("Deploy failed: " + id + ": " + error);
  for (const auto &id : ids) {
    auto session = engine.CreateSession(&error);
    if (!session || !engine.Configure(session, id, false, false, false, &error))
      throw std::runtime_error("Load failed: " + id + ": " + error);
    engine::EngineSnapshot snapshot;
    for (const auto *sample : {"a", "nihao", "1234567890"}) {
      for (const char *key = sample; *key; ++key)
        if (!engine.ProcessKey(session, *key, 0, &snapshot, &error))
          throw std::runtime_error("Input probe failed for " + id + " (" + sample + "): " + error);
      if (!engine.ProcessKey(session, 0xff1b, 0, &snapshot, &error))
        throw std::runtime_error(error);
    }
    engine.DestroySession(session);
  }
  engine.Stop();
  return 0;
}
int ImportSmoke(int argc, wchar_t **argv) {
  if (argc != 3 && argc != 7)
    throw std::runtime_error(
        "Expected an isolated library and optionally ZIP/schema/keys/expected");
  const auto root = fs::absolute(argv[2]);
  if (fs::exists(root))
    throw std::runtime_error("Smoke requires a new isolated directory");
  fs::create_directories(root);
  common::Review review;
  std::string schema, keys = "nihao", expected = "你好";
  if (argc == 7) {
    review = common::Inspect(fs::absolute(argv[3]));
    schema = Utf8(argv[4]);
    keys = Utf8(argv[5]);
    expected = Utf8(argv[6]);
  } else {
    std::ofstream text(root / "fixture.tsv", std::ios::binary);
    text << "你好\tnihao\t100\n世界\tshijie\t50\n";
    text.close();
    review = common::InspectText(root / "fixture.tsv", root / "fixture.zip");
    schema = review.schemes.front().id;
  }
  common::Library library(root / "library");
  auto package = library.Install(review, {schema}, Deploy);
  if (!fs::is_regular_file(library.Root() / "packages" / package.id / "build" / "default.yaml"))
    throw std::runtime_error("Imported workspace is missing deployed default.yaml");
  if (library.Active())
    throw std::runtime_error("Installing must not activate");
  library.Activate(package.id, schema);
  auto selection = common::Library(library.Root()).Active();
  if (!selection)
    throw std::runtime_error("Selection did not survive reload");
  broker::DefaultBrokerPaths paths;
  std::wstring path_error;
  if (!broker::ResolveDefaultBrokerPaths(&paths, &path_error))
    throw std::runtime_error(Utf8(path_error));
  engine::RimeEngineOptions options;
  options.dll_path = paths.dll_path;
  options.shared_data_dir = selection->resources;
  options.user_data_dir = selection->user;
  options.log_dir = root / "logs";
  fs::create_directories(options.log_dir);
  options.verify_input_session = false;
  options.skip_maintenance = true;
  engine::RimeEngine engine;
  std::string error;
  if (!engine.Start(options, &error))
    throw std::runtime_error(error);
  auto session = engine.CreateSession(&error);
  if (!session ||
      !engine.Configure(session, schema, false, false, false, &error))
    throw std::runtime_error(error);
  engine::EngineSnapshot snapshot;
  for (const char key : keys)
    if (!engine.ProcessKey(session, key, 0, &snapshot, &error))
      throw std::runtime_error(error);
  if (std::none_of(
          snapshot.candidates.begin(), snapshot.candidates.end(),
          [&](const auto &candidate) { return candidate.text == expected; }))
    throw std::runtime_error("Expected imported candidate missing");
  if (!engine.ProcessKey(session, 32, 0, &snapshot, &error) ||
      snapshot.commit_text != expected)
    throw std::runtime_error("Unexpected imported commit");
  engine.DestroySession(session);
  engine.Stop();
  std::ofstream marker(selection->user / "learning-marker.txt");
  marker << "keep";
  marker.close();
  library.Deactivate();
  library.Remove(package.id);
  if (!library.Packages().empty() ||
      !fs::exists(selection->user / "learning-marker.txt"))
    throw std::runtime_error("Restore/remove changed learning data");
  std::cout << "PASS native scheme import, deploy, candidate, commit, reload, "
               "restore and removal; SHA256="
            << package.source_digest << '\n';
  return 0;
}
constexpr UINT kResult = WM_APP + 42;
struct Manager;
struct Result {
  Manager *owner;
  std::optional<common::Review> review;
  std::wstring message;
  fs::path temporary;
};
struct Manager {
  common::Library library{Library()};
  HWND window = nullptr, list = nullptr, status = nullptr,
       import_button = nullptr, activate = nullptr, restore = nullptr,
       remove = nullptr;
  HFONT font = nullptr;
  bool busy = false;
  std::optional<common::Review> review;
  fs::path temporary;
  std::vector<std::pair<std::string, std::string>> rows;
  ~Manager() {
    if (font)
      DeleteObject(font);
    std::error_code ignored;
    if (!temporary.empty())
      fs::remove_all(temporary, ignored);
  }
  void Message(const std::wstring &text) {
    SetWindowTextW(status, text.c_str());
  }
  void Busy(bool value) {
    busy = value;
    for (auto control : {import_button, activate, restore, remove, list})
      EnableWindow(control, !value);
  }
  void Refresh() {
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    rows.clear();
    auto active = library.Active();
    for (const auto &package : library.Packages())
      for (const auto &schema : package.schemes) {
        auto text =
            Wide(package.name + " · " + schema.name + " (" + schema.id + ")");
        if (active && active->package.id == package.id &&
            active->schema == schema.id)
          text += L"  ✓";
        SendMessageW(list, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(text.c_str()));
        rows.emplace_back(package.id, schema.id);
      }
  }
  void EndReview() {
    review.reset();
    if (!temporary.empty()) {
      std::error_code ignored;
      fs::remove_all(temporary, ignored);
      temporary.clear();
    }
    SetWindowTextW(import_button, L"导入文件…");
    SetWindowTextW(activate, L"启用所选");
    EnableWindow(restore, TRUE);
    EnableWindow(remove, TRUE);
    Refresh();
  }
  void Choose() {
    if (review) {
      EndReview();
      Message(L"已取消，原方案未改变。");
      return;
    }
    wchar_t path[32768]{};
    OPENFILENAMEW picker{};
    picker.lStructSize = sizeof(picker);
    picker.hwndOwner = window;
    picker.lpstrFilter = L"Rime ZIP / TXT / TSV\0*.zip;*.txt;*.tsv\0\0";
    picker.lpstrFile = path;
    picker.nMaxFile = 32768;
    picker.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&picker))
      return;
    Busy(true);
    Message(L"正在检查文件…");
    std::thread([this, source = fs::path(path)] {
      auto result = new Result{this, {}, {}, {}};
      try {
        wchar_t temp[32768]{};
        if (!GetTempPathW(32768, temp))
          throw std::runtime_error("Cannot create staging directory");
        result->temporary =
            fs::path(temp) /
            (L"rimes-schemes-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()));
        fs::create_directory(result->temporary);
        auto extension = source.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       ::towlower);
        result->review =
            extension == L".txt" || extension == L".tsv"
                ? common::InspectText(source, result->temporary / L"text.zip")
                : common::Inspect(source);
      } catch (const std::exception &e) {
        result->message = Wide(e.what());
      }
      PostMessageW(window, kResult, 0, reinterpret_cast<LPARAM>(result));
    }).detach();
  }
  void Apply() {
    if (review) {
      std::vector<std::string> selected;
      for (std::size_t i = 0; i < review->schemes.size(); ++i)
        if (SendMessageW(list, LB_GETSEL, i, 0) > 0)
          selected.push_back(review->schemes[i].id);
      if (selected.empty()) {
        Message(L"请选择至少一个方案。可使用 Ctrl 多选。");
        return;
      }
      Busy(true);
      Message(L"正在编译并验证方案，请稍候…");
      std::thread([this, selected, inspection = *review] {
        auto result =
            new Result{this, {}, L"已导入。请选择一个方案并启用。", {}};
        try {
          library.Install(inspection, selected, Deploy);
        } catch (const std::exception &e) {
          result->message = Wide(e.what());
        }
        PostMessageW(window, kResult, 1, reinterpret_cast<LPARAM>(result));
      }).detach();
    } else
      Change(1);
  }
  void Change(int action) {
    try {
      if (!action)
        library.Deactivate();
      else {
        int index = -1;
        if (SendMessageW(list, LB_GETSELCOUNT, 0, 0) != 1 ||
            SendMessageW(list, LB_GETSELITEMS, 1,
                         reinterpret_cast<LPARAM>(&index)) != 1) {
          Message(L"请选择一个方案。");
          return;
        }
        const auto selected = rows.at(static_cast<std::size_t>(index));
        if (action == 1)
          library.Activate(selected.first, selected.second);
        else {
          if (MessageBoxW(window,
                          rimes::branding::Text(L"删除整个方案包？学习记录保留。请先退出正在使用该包"
                                                L"的 {brand}。").c_str(),
                          L"方案管理", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return;
          UsageLock unused(library.Root() / "learning" / selected.first, true);
          library.Remove(selected.first);
        }
      }
      Refresh();
      Message(action == 2 ? L"已删除方案包，学习记录保留。"
                          : rimes::branding::Text(L"选择已保存。请从托盘退出 "
                                                  L"{brand}，再打开设置或切换输入法使其生效。"));
    } catch (const std::exception &e) {
      Message(Wide(e.what()));
    }
  }
  void Layout() {
    RECT rect{};
    GetClientRect(window, &rect);
    auto dpi = GetDpiForWindow(window);
    auto scale = [dpi](int n) { return MulDiv(n, static_cast<int>(dpi), 96); };
    const int width = rect.right, height = rect.bottom;
    MoveWindow(list, scale(16), scale(16), width - scale(32),
               (std::max)(scale(100), height - scale(162)), TRUE);
    int x = scale(16);
    for (auto control : {import_button, activate, restore, remove}) {
      MoveWindow(control, x, height - scale(130), scale(140), scale(34), TRUE);
      x += scale(150);
    }
    MoveWindow(status, scale(16), height - scale(84), width - scale(32),
               scale(72), TRUE);
  }
  static LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam) {
    auto self =
        reinterpret_cast<Manager *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      self = static_cast<Manager *>(
          reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
      self->window = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
      return DefWindowProcW(hwnd, message, wparam, lparam);
    if (message == WM_COMMAND && HIWORD(wparam) == BN_CLICKED && !self->busy) {
      switch (LOWORD(wparam)) {
      case 101:
        self->Choose();
        break;
      case 102:
        self->Apply();
        break;
      case 103:
        self->Change(0);
        break;
      case 104:
        self->Change(2);
        break;
      default:
        break;
      }
      return 0;
    }
    if (message == kResult) {
      std::unique_ptr<Result> result(reinterpret_cast<Result *>(lparam));
      self->Busy(false);
      if (result->review) {
        self->review = std::move(result->review);
        self->temporary = std::move(result->temporary);
        SendMessageW(self->list, LB_RESETCONTENT, 0, 0);
        bool recommended = std::any_of(
            self->review->schemes.begin(), self->review->schemes.end(),
            [](const auto &s) { return s.recommended; });
        for (const auto &schema : self->review->schemes) {
          auto text = Wide(schema.name + " (" + schema.id + ")");
          auto index = SendMessageW(self->list, LB_ADDSTRING, 0,
                                    reinterpret_cast<LPARAM>(text.c_str()));
          if (schema.recommended || (!recommended && index == 0))
            SendMessageW(self->list, LB_SETSEL, TRUE, index);
        }
        SetWindowTextW(self->import_button, L"取消导入");
        SetWindowTextW(self->activate, L"编译并导入");
        EnableWindow(self->restore, FALSE);
        EnableWindow(self->remove, FALSE);
        std::wstring notice =
            L"选择要导入的方案（Ctrl 多选）。原文件不会被修改。";
        for (const auto &item : self->review->warnings)
          notice += L"\r\n" + Wide(item);
        self->Message(notice);
      } else {
        if (!result->temporary.empty()) {
          std::error_code ignored;
          fs::remove_all(result->temporary, ignored);
        }
        if (wparam)
          self->EndReview();
        self->Message(result->message);
      }
      return 0;
    }
    if (message == WM_SIZE) {
      if (self->list)
        self->Layout();
      return 0;
    }
    if (message == WM_CLOSE && self->busy)
      return 0;
    if (message == WM_DESTROY) {
      PostQuitMessage(0);
      return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }
  int Run() {
    FreeConsole();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW type{};
    type.lpfnWndProc = Procedure;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"Rimes.SchemeManager";
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&type);
    window = CreateWindowExW(0, type.lpszClassName, rimes::branding::Text(L"{brand} · 方案管理").c_str(),
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             900, 650, nullptr, nullptr, type.hInstance, this);
    if (!window)
      return 1;
    font =
        CreateFontW(-MulDiv(15, static_cast<int>(GetDpiForWindow(window)), 96),
                    0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH, L"Microsoft YaHei UI");
    list =
        CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                            LBS_EXTENDEDSEL | LBS_NOINTEGRALHEIGHT,
                        0, 0, 1, 1, window, nullptr, type.hInstance, nullptr);
    auto button = [&](const wchar_t *title, int id) {
      return CreateWindowExW(0, L"BUTTON", title,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                             0, 0, 1, 1, window,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             type.hInstance, nullptr);
    };
    import_button = button(L"导入文件…", 101);
    activate = button(L"启用所选", 102);
    restore = button(L"恢复内置", 103);
    remove = button(L"删除方案包", 104);
    status = CreateWindowExW(
        0, L"EDIT",
        rimes::branding::Text(L"导入 Rime ZIP，或带编码的 UTF-8 TXT / TSV 词库。启用或恢复方案后重启 "
                              L"{brand} 生效。").c_str(),
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY, 0, 0,
        1, 1, window, nullptr, type.hInstance, nullptr);
    for (auto control :
         {list, status, import_button, activate, restore, remove})
      SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    Refresh();
    Layout();
    ShowWindow(window, SW_SHOWNORMAL);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
      if (!IsDialogMessageW(window, &message)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
    return 0;
  }
};
} // namespace
int RunManager(int argc, wchar_t **argv) {
  try {
    if (argc >= 3 && std::wstring(argv[1]) == L"--scheme-import-smoke")
      return ImportSmoke(argc, argv);
    if (argc >= 4 && std::wstring(argv[1]) == L"--scheme-deploy") {
      std::vector<std::string> ids;
      for (int i = 3; i < argc; ++i)
        ids.push_back(Utf8(argv[i]));
      return Compile(fs::absolute(argv[2]), ids);
    }
    Manager manager;
    return manager.Run();
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    if (argc == 2)
      MessageBoxW(nullptr, Wide(e.what()).c_str(), L"方案管理",
                  MB_OK | MB_ICONERROR);
    return 1;
  }
}
} // namespace rimes::windows::schemes
