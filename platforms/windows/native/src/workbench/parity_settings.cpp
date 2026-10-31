#include "parity_settings.hpp"
#include "chord_profile.hpp"
#include "codex_cli.hpp"
#include "local_gateway.hpp"
#include "../ui/settings_scrollbar.hpp"
#include <atomic>
#include <set>
#include <commctrl.h>
#include <commdlg.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <objbase.h>
#include <thread>
#include <cmath>

namespace rimes::windows::workbench {
namespace {
std::wstring Text(HWND h) {
  const auto n = GetWindowTextLengthW(h);
  std::wstring s(static_cast<std::size_t>(n) + 1, 0);
  GetWindowTextW(h, s.data(), n + 1);
  s.resize(n);
  return s;
}
void Set(HWND h, const std::wstring &s) { if (h && Text(h) != s) SetWindowTextW(h, s.c_str()); }
unsigned Number(HWND h) {
  const auto s = Text(h);
  std::size_t n = 0;
  auto v = std::stoul(s, &n);
  if (n != s.size() || v > 100000)
    throw std::runtime_error("数字格式无效。");
  return static_cast<unsigned>(v);
}
std::string ID() {
  GUID id{};
  if (FAILED(CoCreateGuid(&id)))
    throw std::runtime_error("无法新建配置。");
  wchar_t s[40]{};
  StringFromGUID2(id, s, 40);
  auto value = Utf8(std::wstring(s + 1, 36));
  for (auto &c : value)
    if (c >= 'A' && c <= 'F')
      c = static_cast<char>(c - 'A' + 'a');
  return value;
}
} // namespace
struct ParitySettings::Impl {
  HWND parent = nullptr;
  Settings config;
  ConnectorUiServices services;
  ui::SettingsPage page = ui::SettingsPage::kHome;
  int subpage = 0, scroll = 0, scroll_max = 0;
  unsigned dpi = 96;
  ui::DipRect viewport{}, panel{}, route{}, spinner{};
  bool gateway_enabled = true, advanced = false, auth_known = false, auth_ready = false;
  std::string auth_identity;
  std::map<std::string, std::pair<std::string, bool>> stored_keys;
  std::string login_url, last_api_id;
  std::wstring feedback;
  bool feedback_error = false;
  std::map<unsigned, HWND> controls;
  std::vector<ui::DipRect> edit_frames;
  std::map<std::string, std::wstring> secrets;
  std::map<std::string, std::wstring> saved_secrets;
  std::set<std::string> clear_secrets;
  std::size_t selected = 0;
  core::Json profile;
  struct Probe {
    std::mutex mutex;
    bool done = false;
    std::string id;
    ConnectionProbeResult result;
    std::atomic<bool> cancelled = false;
    bool login = false;
    std::string url, identity;
  };
  std::shared_ptr<Probe> probe;
  bool syncing = false;
  unsigned generation = 0, probe_generation = 0;
  unsigned gateway_client = 0;
  std::wstring gateway_status;
  HFONT gateway_font = nullptr;
  unsigned gateway_dpi = 0;
  void ShowGateway() {
    auto multiline = [](std::wstring text) {
      for (std::size_t i = 0; (i = text.find(L'\n', i)) != std::wstring::npos; i += 2) text.insert(i, 1, L'\r');
      return text;
    };
    Set(H(6055), multiline(GatewayConfiguration(gateway_client)));
    Set(H(6057), multiline(GatewayConfiguration(gateway_client, false, true)));
    for (unsigned i = 0; i < 4; ++i) {
      if (i == gateway_client) SetPropW(H(6051 + i), L"RimesSelectedSegment", reinterpret_cast<HANDLE>(1));
      else RemovePropW(H(6051 + i), L"RimesSelectedSegment");
      InvalidateRect(H(6051 + i), nullptr, FALSE);
    }
    const wchar_t* details[] = {L"粘贴到项目的 .cursor/mcp.json 或用户级 %USERPROFILE%/.cursor/mcp.json。",
        L"粘贴到 %USERPROFILE%/.codex/config.toml；令牌通过环境变量提供。",
        L"粘贴到项目的 .mcp.json，或复制下方命令在 PowerShell 注册。",
        L"适用于接受 Streamable HTTP MCP JSON 配置的客户端。"};
    Set(H(6151), details[gateway_client]);
    Set(H(6154), gateway_client == 1 ? L"环境变量（预览已脱敏）" : L"PowerShell 命令（可选，预览已脱敏）");
    Set(H(6058), gateway_client == 1 ? L"复制环境变量" : L"复制终端命令");
  }
  ~Impl() {
    if (parent) KillTimer(parent, 13);
    if (gateway_font) DeleteObject(gateway_font);
    if (probe) probe->cancelled = true;
    for (auto &[id, key] : secrets)
      if (!key.empty())
        SecureZeroMemory(key.data(), key.size() * sizeof(wchar_t));
    for (auto &[id, key] : saved_secrets)
      if (!key.empty())
        SecureZeroMemory(key.data(), key.size() * sizeof(wchar_t));
  }
  HWND H(unsigned id) const {
    auto it = controls.find(id);
    return it == controls.end() ? nullptr : it->second;
  }
  HWND Add(unsigned id, const wchar_t *type, const wchar_t *text, DWORD style) {
    const auto h =
        CreateWindowExW(0, type, text, WS_CHILD | style, 0, 0, 1, 1, parent,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                        GetModuleHandleW(nullptr), nullptr);
    controls[id] = h;
    return h;
  }
  void Label(unsigned id, const wchar_t *text) {
    Add(id, L"STATIC", text, SS_LEFT);
    if (id != 6161) SetPropW(H(id), L"RimesSecondary", reinterpret_cast<HANDLE>(1));
  }
  void Edit(unsigned id, bool secret = false) {
    Add(id, L"EDIT", L"",
        WS_TABSTOP | ES_AUTOHSCROLL | (secret ? ES_PASSWORD : 0));
  }
  void Button(unsigned id, const wchar_t *text) {
    Add(id, L"BUTTON", text, WS_TABSTOP | BS_OWNERDRAW);
  }
  void Combo(unsigned id, std::initializer_list<const wchar_t *> entries = {},
             bool editable = false) {
    auto h = Add(id, L"COMBOBOX", L"",
                 WS_TABSTOP | WS_VSCROLL | WS_CLIPCHILDREN |
                     (editable ? CBS_DROPDOWN : CBS_DROPDOWNLIST) |
                     CBS_HASSTRINGS | CBS_OWNERDRAWFIXED);
    SetWindowLongPtrW(h, GWL_STYLE,
                      GetWindowLongPtrW(h, GWL_STYLE) & ~WS_BORDER);
    SetWindowPos(h, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE);
    for (const auto *text : entries)
      SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
  }
  void Status(const std::wstring &text, bool error = false) {
    feedback = text; feedback_error = error;
    if (error) SetPropW(H(6099), L"RimesError", reinterpret_cast<HANDLE>(1)); else RemovePropW(H(6099), L"RimesError");
    Set(H(6099), text);
  }
  bool HasKey() const {
    const auto& c = config.connections.at(selected);
    if (GetWindowTextLengthW(H(6008))) return true;
    if (clear_secrets.contains(c.id)) return false;
    const auto found = stored_keys.find(c.id);
    return found != stored_keys.end() && found->second.first == Utf8(Text(H(6006))) && found->second.second;
  }
  bool EndpointReady() const { return ValidConnectionURL(Utf8(Text(H(6006)))); }
  bool KeyReady() const {
    const auto endpoint = Utf8(Text(H(6006)));
    return HasKey() || (ValidConnectionURL(endpoint) && endpoint.starts_with("http://"));
  }
  bool TextReady() const { return EndpointReady() && KeyReady() && !Text(H(6007)).empty(); }
  void RefreshControls() {
    const auto& c = config.connections.at(selected);
    const bool api = c.kind != "codex-cli", busy = probe != nullptr;
    for (unsigned i = 0; i < 3; ++i) {
      const auto h = H(6060 + i);
      if ((api && i == 2) || (!api && i == 0)) SetPropW(h, L"RimesSelectedSegment", reinterpret_cast<HANDLE>(1));
      else RemovePropW(h, L"RimesSelectedSegment");
      EnableWindow(h, i != 1);
      InvalidateRect(h, nullptr, FALSE);
    }
    EnableWindow(H(6009), !busy && services.probe && (!api || (EndpointReady() && KeyReady())));
    EnableWindow(H(6041), api && HasKey());
    EnableWindow(H(6042), api && TextReady() && c.id != config.active_connection);
    EnableWindow(H(6064), ValidConnectionURL(Utf8(Text(H(6063))), true) && services.open_url);
    EnableWindow(H(6040), services.login && (!busy || (probe->login && !probe->cancelled)));
    Set(H(6040), probe && probe->login ? (probe->cancelled ? L"正在取消…" : L"取消登录") : auth_ready ? L"重新登录 ChatGPT" : L"登录 ChatGPT");
    Set(H(6160), busy && !api ? (probe->login ? L"等待授权" : L"正在检查…") : auth_known ? (auth_ready ? L"可用" : L"不可用") : L"尚未检查");
    if (auth_ready) SetPropW(H(6160), L"RimesAccent", reinterpret_cast<HANDLE>(1)); else RemovePropW(H(6160), L"RimesAccent");
    Set(H(6162), HasKey() ? L"API Key 已保存或已填写（脱敏）" : L"尚未保存 API Key；本机 HTTP 服务可不填。");
    Set(H(6163), c.id == config.active_connection ? (TextReady() ? L"当前文本模型" : L"当前文本模型 · 尚未完成配置") : TextReady() ? L"可设为当前文本模型" : L"待配置：填写地址、模型与所需密钥");
    Set(H(6067), advanced ? L"收起高级配置" : L"高级配置…");
  }
  void BeginProbe(bool login) {
    if (probe || (login ? !services.login : !services.probe)) return;
    CaptureConnection();
    const auto c = config.connections.at(selected);
    auto state = std::make_shared<Probe>(); state->id = c.id; state->identity = c.id + "|" + c.executable;
    state->login = login; probe = state; probe_generation = generation; login_url.clear();
    Status(login ? L"正在启动 ChatGPT 授权…" : L"正在检查连接…");
    SetTimer(parent, 13, 80, nullptr);
    auto key = secrets.contains(c.id) ? secrets.at(c.id) : saved_secrets.contains(c.id) ? saved_secrets.at(c.id) : std::wstring();
    const auto service = services;
    std::thread([state, c, service, key = std::move(key)]() mutable {
      ConnectionProbeResult result;
      try {
        if (state->login) {
          std::string error;
          result.success = service.login(c, [state] { return state->cancelled.load(); },
              [state](const std::string& url) { std::lock_guard lock(state->mutex); if (!state->cancelled) state->url = url; }, &error);
          if (result.success && service.probe) result = service.probe(c, L"");
          result.message = result.success ? "ChatGPT 授权成功，连接器已就绪。" : state->cancelled ? "已取消登录。" : "授权未完成，请重试或检查高级配置。";
        } else result = service.probe(c, key);
      } catch (...) { result.message = "连接检查失败，请检查配置后重试。"; }
      if (!key.empty()) SecureZeroMemory(key.data(), key.size() * sizeof(wchar_t));
      std::lock_guard lock(state->mutex); state->result = std::move(result); state->done = true;
    }).detach();
    RefreshControls();
  }
  void CaptureConnection() {
    if (config.connections.empty())
      return;
    auto &p = config.connections.at(selected);
    p.name = Utf8(Text(H(6004)));
    if (p.kind == "codex-cli")
      p.executable = Utf8(Text(H(6006)));
    else
      p.base_url = Utf8(Text(H(6006)));
    p.model = Utf8(Text(H(6007)));
    if (p.kind != "codex-cli") p.documentation_url = Utf8(Text(H(6063)));
    auto key = Text(H(6008));
    if (p.kind == "openai-compatible") {
      auto prior = secrets.find(p.id);
      if (prior != secrets.end()) {
        if (!prior->second.empty())
          SecureZeroMemory(prior->second.data(),
                           prior->second.size() * sizeof(wchar_t));
        secrets.erase(prior);
      }
      // A blank field preserves the credential already in the vault, not a
      // draft the user typed and subsequently erased in this dialog.
      if (!key.empty() && (!saved_secrets.contains(p.id) || saved_secrets.at(p.id) != key))
        secrets.emplace(p.id, key);
      if (!key.empty()) clear_secrets.erase(p.id);
      else if (clear_secrets.contains(p.id)) secrets[p.id] = L"";
    }
    if (!key.empty())
      SecureZeroMemory(key.data(), key.size() * sizeof(wchar_t));
  }
  void ShowConnection() {
    syncing = true;
    SendMessageW(H(6000), CB_RESETCONTENT, 0, 0);
    unsigned picker_index = 0;
    for (std::size_t index = 0; index < config.connections.size(); ++index) {
      const auto& p = config.connections[index];
      if (p.kind != "openai-compatible") continue;
      const auto text = Wide(p.name);
      const auto item = SendMessageW(H(6000), CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(text.c_str()));
      SendMessageW(H(6000), CB_SETITEMDATA, item, index);
      if (index == selected) picker_index = static_cast<unsigned>(item);
    }
    SendMessageW(H(6000), CB_SETCURSEL, picker_index, 0);
    const auto &p = config.connections.at(selected);
    Set(H(6004), Wide(p.name));
    if (p.kind != "codex-cli") last_api_id = p.id;
    if (services.has_secret && p.kind != "codex-cli") stored_keys[p.id] = {p.base_url, services.has_secret(p)};
    Set(H(6063), Wide(p.documentation_url));
    Set(H(6006), Wide(p.kind == "codex-cli" ? p.executable : p.base_url));
    SendMessageW(H(6007), CB_RESETCONTENT, 0, 0);
    Set(H(6007), Wide(p.model));
    Set(H(6008), secrets.contains(p.id) ? secrets.at(p.id)
        : saved_secrets.contains(p.id) ? saved_secrets.at(p.id) : L"");
    EnableWindow(H(6008), p.kind != "codex-cli");
    Set(H(6106), p.kind == "codex-cli" ? L"Codex 路径" : L"API 地址");
    Set(H(6009), p.kind == "codex-cli" ? L"检查授权状态" : L"查询模型 / 测试连接");
    Set(H(6110), p.kind == "codex-cli"
        ? L"使用输入法专用的 ChatGPT 登录；不会读取个人 Codex 的 MCP、工具、Hook 或技能。"
        : L"每个 Provider 独立保存地址、模型与密钥；选择当前文本模型后供 AI 插件使用。");
    const auto identity = p.id + "|" + p.executable;
    if (p.kind == "codex-cli" && auth_identity != identity) { auth_identity = identity; auth_known = auth_ready = false; }
    syncing = false;
    RefreshControls();
  }
  void ShowMappings() {
    SendMessageW(H(6020), LB_RESETCONTENT, 0, 0);
    for (const auto &row : profile.at("mappings")) {
      const auto line =
          Wide(row.at("keys").get<std::string>() + "   →   " +
               row.at("output").get<std::string>() +
               (row.at("kind") == "syllable" ? "  · 音节" : "  · 片段"));
      SendMessageW(H(6020), LB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(line.c_str()));
    }
    Set(H(6010), Wide(profile.value("name", "isaac2026")));
    Set(H(6014), Wide(profile.at("leftKeys")));
    Set(H(6015), Wide(profile.at("rightKeys")));
  }
  void CaptureProfile() {
    profile["name"] = Utf8(Text(H(6010)));
    profile["leftKeys"] = Utf8(Text(H(6014)));
    profile["rightKeys"] = Utf8(Text(H(6015)));
  }
  void File(bool save) {
    CaptureProfile();
    wchar_t path[32768] = L"isaac2026-keymap.json";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = parent;
    dialog.lpstrFilter = L"并击键位 JSON\0*.json\0\0";
    dialog.lpstrDefExt = L"json";
    dialog.lpstrFile = path;
    dialog.nMaxFile = 32768;
    dialog.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
                   (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog)))
      return;
    const std::filesystem::path file(path);
    if (std::filesystem::is_symlink(file))
      throw std::runtime_error("不能使用符号链接。");
    if (save) {
      (void)ChordProfile::Parse(profile);
      auto temporary = file;
      temporary += L".xsime-" + Wide(ID()) + L".tmp";
      std::ofstream out(temporary, std::ios::binary);
      out << profile.dump(2);
      out.close();
      if (!out ||
          !MoveFileExW(temporary.c_str(), file.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("导出失败。");
      }
      Status(L"已导出当前键位配置。");
    } else {
      if (!std::filesystem::is_regular_file(file) ||
          std::filesystem::file_size(file) > 2 * 1024 * 1024)
        throw std::runtime_error("键位文件过大或类型无效。");
      std::ifstream in(file, std::ios::binary);
      auto imported = core::Json::parse(
          in, [](int depth, core::Json::parse_event_t, core::Json &) {
            if (depth > 16)
              throw std::runtime_error("键位文件嵌套过深。");
            return true;
          });
      (void)ChordProfile::Parse(imported);
      imported["id"] = ID();
      profile = std::move(imported);
      ShowMappings();
      Status(L"已导入键位配置。");
    }
  }
};
ParitySettings::ParitySettings() : impl_(std::make_unique<Impl>()) {}
ParitySettings::~ParitySettings() = default;
void ParitySettings::Create(HWND parent, const Settings &settings, ConnectorUiServices services) {
  auto &p = *impl_;
  p.parent = parent;
  p.config = settings;
  p.services = std::move(services);
  p.gateway_enabled = settings.gateway_enabled;
  MigrateConnections(&p.config);
  for (std::size_t i = 0; i < p.config.connections.size(); ++i)
    if (p.config.connections[i].id == p.config.active_connection)
      p.selected = i;
  p.profile = settings.chord_profile.is_null() ? BuiltinChordProfile()
                                               : settings.chord_profile;
  p.Combo(6000);
  p.Button(6001, L"新建");
  p.Button(6002, L"添加 OpenRouter");
  p.Button(6003, L"删除");
  p.Edit(6004);
  p.Button(6060, L"ChatGPT"); p.Button(6061, L"Claude · 暂未实现"); p.Button(6062, L"AI Provider");
  p.Edit(6063); p.Button(6064, L"打开文档"); p.Button(6065, L"复制登录链接");
  p.Button(6067, L"高级配置…");
  p.Label(6160, L"尚未检查"); p.Label(6161, L"Codex CLI");
  p.Label(6162, L""); p.Label(6163, L"");
  p.Label(6164, L"模型路由");
  p.Label(6165, L"OpenAI Chat Completions · 文本生成 / 流式文本");
  p.Label(6166, L"Native Decisions / Jev · Windows 暂未实现，已禁用");
  // This is a noninteractive capability label. WS_DISABLED makes STATIC use
  // Windows' embossed gray text instead of the application's theme palette.
  SetPropW(p.H(6166), L"RimesMuted", reinterpret_cast<HANDLE>(1));
  p.Add(6070, L"SCROLLBAR", L"", SBS_VERT);
  p.Label(6109, L"文档");
  p.Edit(6006);
  p.Combo(6007, {}, true);
  p.Edit(6008, true);
  p.Button(6009, L"读取模型列表");
  p.Button(6040, L"登录 ChatGPT");
  p.Button(6041, L"清除密钥");
  p.Button(6042, L"设为当前文本模型");
  p.Button(6050, L"启用本地网关");
  p.Button(6051, L"Cursor"); p.Button(6052, L"Codex");
  p.Button(6053, L"Claude Code"); p.Button(6054, L"通用 MCP");
  p.Add(6055, L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | WS_VSCROLL);
  p.Button(6056, L"复制配置");
  p.Add(6057, L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | WS_VSCROLL);
  p.Button(6058, L"复制环境变量");
  p.Label(6150, L"允许本机智能体通过 MCP / HTTP 推送内容至 Mailbox，等待确认后上屏。\n配置仅指向 127.0.0.1；预览不会读取或显示真实令牌。");
  p.Label(6151, L""); p.Label(6152, L"配置预览（令牌已脱敏）");
  p.Label(6153, L""); p.Label(6154, L"");
  p.ShowGateway();
  p.Label(6104, L"名称");
  p.Label(6105, L"连接方式");
  p.Label(6106, L"API 地址");
  p.Label(6107, L"模型");
  p.Label(6108, L"API Key");
  p.Label(
      6110,
      L"选为当前文本模型后，该配置用于 AI 生成与 AI 翻译。每个 API 的密钥独立存储。\nCodex "
      L"路径和模型可留空；使用输入法专用的 ChatGPT 授权。");
  p.Edit(6010);
  p.Combo(6011, {L"开启并击", L"关闭并击 · 使用全拼"});
  p.Edit(6012);
  p.Button(6013, L"恢复 isaac2026");
  p.Edit(6014);
  p.Edit(6015);
  p.Button(6016, L"导入 JSON…");
  p.Button(6017, L"导出 JSON…");
  p.Label(6111, L"键位名称");
  p.Label(6112, L"并击开关");
  p.Label(6113, L"组键间隔（10–300 ms）");
  p.Label(6114, L"左手键区");
  p.Label(6115, L"右手键区");
  p.Label(6116, L"选择输入法 → isaac2026 "
                L"后使用。左右相邻批次可按完整音节映射重组。\n编辑、焦点或方案"
                L"变化会结束配对；有效修改自动生效。");
  p.Add(6020, L"LISTBOX", L"",
        WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT);
  p.Edit(6021);
  p.Edit(6022);
  p.Combo(6023, {L"完整音节", L"拼音片段"});
  p.Button(6024, L"新增 / 更新映射");
  p.Button(6025, L"删除所选");
  p.Label(6121, L"组合键，例如 dv");
  p.Label(6122, L"全拼输出，例如 n");
  p.Edit(6026);
  p.Button(6027, L"检查组合");
  p.Label(6126, L"组合预览：输入组合键，检查映射输出；不写入宿主或词库。");
  p.Edit(6030);
  p.Edit(6031);
  p.Combo(6032, {L"固定所选主题", L"跟随 Windows 明暗"});
  p.Edit(6033);
  p.Edit(6034);
  p.Edit(6035);
  p.Label(6130, L"工作台默认宽度（520–1100）");
  p.Label(6131, L"长按发送全部（500–3000 ms）");
  p.Label(6132, L"系统外观");
  p.Label(6133, L"候选最大宽度（240–1200）");
  p.Label(6134, L"候选条最小高度（28–72）");
  p.Label(6135, L"序号字号（8–24）");
  p.Label(6099, L"");
  Set(p.H(6030), std::to_wstring(settings.buffer_width));
  Set(p.H(6031), std::to_wstring(settings.send_hold_ms));
  Set(p.H(6033), std::to_wstring(settings.candidate_width));
  Set(p.H(6034), std::to_wstring(settings.candidate_height));
  Set(p.H(6035), std::to_wstring(settings.candidate_label_size));
  SendMessageW(p.H(6032), CB_SETCURSEL, settings.follow_system_theme ? 1 : 0,
               0);
  SendMessageW(p.H(6011), CB_SETCURSEL, settings.chord_enabled ? 0 : 1, 0);
  Set(p.H(6012), std::to_wstring(settings.chord_interval_ms));
  SendMessageW(p.H(6023), CB_SETCURSEL, 0, 0);
  p.ShowConnection();
  p.ShowMappings();
}
void ParitySettings::Layout(ui::SettingsPage page, int subpage,
                            const ui::DipRect &body, unsigned dpi, HFONT font) {
  auto &p = *impl_;
  p.edit_frames.clear();
  if (p.page != page || p.subpage != subpage) p.scroll = 0;
  p.page = page; p.subpage = subpage; p.viewport = body; p.dpi = dpi;
  p.panel = {}; p.route = {}; p.spinner = {};
  std::set<unsigned> shown;
  for (auto [id, h] : p.controls) {
    const auto desired_font = (id == 6055 || id == 6057) ? p.gateway_font : font;
    if (reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)) != desired_font)
      SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(desired_font), FALSE);
  }
  const bool connector = page == ui::SettingsPage::kApi;
  const bool codex = p.config.connections[p.selected].kind == "codex-cli";
  const float height = subpage == 1 ? 472.f : codex ? (p.advanced ? 340.f : 238.f) : 512.f;
  p.scroll_max = connector ? (std::max)(0, static_cast<int>(height - body.height())) : 0;
  p.scroll = (std::clamp)(p.scroll, 0, p.scroll_max);
  const float x = body.left, y = body.top - p.scroll, w = body.width();
  if (!p.gateway_font || p.gateway_dpi != dpi) {
    if (p.gateway_font) DeleteObject(p.gateway_font);
    p.gateway_dpi = dpi;
    p.gateway_font = CreateFontW(-MulDiv(10, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
  }
  for (const unsigned id : {6055U, 6057U})
    if (reinterpret_cast<HFONT>(SendMessageW(p.H(id), WM_GETFONT, 0, 0)) != p.gateway_font)
      SendMessageW(p.H(id), WM_SETFONT, reinterpret_cast<WPARAM>(p.gateway_font), FALSE);
  auto put = [&](unsigned id, float left, float top, float width,
                 float height) {
    const float scale = static_cast<float>(dpi) / 96;
    wchar_t kind[20]{};
    GetClassNameW(p.H(id), kind, 20);
    if (_wcsicmp(kind, L"EDIT") == 0) {
      p.edit_frames.push_back(
          {x + left, y + top, x + left + width, y + top + height});
      left += 8;
      top += 6;
      width -= 16;
      height -= 11;
    }
    MoveWindow(p.H(id), static_cast<int>((x + left) * scale),
               static_cast<int>((y + top) * scale),
               static_cast<int>(width * scale),
               static_cast<int>(height * scale), FALSE);
    if (std::wstring(kind) == L"ComboBox") {
      SendMessageW(p.H(id), CB_SETITEMHEIGHT, static_cast<WPARAM>(-1),
                   static_cast<LPARAM>(23 * scale));
      SendMessageW(p.H(id), CB_SETITEMHEIGHT, 0,
                   static_cast<LPARAM>(23 * scale));
      if ((GetWindowLongPtrW(p.H(id), GWL_STYLE) & 3) == CBS_DROPDOWN) {
        COMBOBOXINFO info{sizeof(info)};
        RECT client{};
        if (GetComboBoxInfo(p.H(id), &info) &&
            GetClientRect(p.H(id), &client)) {
          const int inset = static_cast<int>(8 * scale);
          const int text_height = static_cast<int>(17 * scale);
          SetWindowPos(info.hwndItem, nullptr, inset,
                       (client.bottom - text_height) / 2,
                       client.right - static_cast<int>(36 * scale), text_height,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        }
      }
    }
    if (connector) {
      const float clipped_top = (std::max)(0.f, body.top - (y + top));
      const float clipped_bottom = (std::min)(height, body.bottom - (y + top));
      if (clipped_bottom <= clipped_top) return;
      const auto region = CreateRectRgn(0, static_cast<int>(clipped_top * scale),
          static_cast<int>(width * scale + 1), static_cast<int>(clipped_bottom * scale + 1));
      const auto old = CreateRectRgn(0, 0, 0, 0);
      if (GetWindowRgn(p.H(id), old) != ERROR && EqualRgn(old, region)) DeleteObject(region);
      else if (!SetWindowRgn(p.H(id), region, FALSE)) DeleteObject(region);
      DeleteObject(old);
    } else {
      const auto old = CreateRectRgn(0, 0, 0, 0);
      if (GetWindowRgn(p.H(id), old) != ERROR) SetWindowRgn(p.H(id), nullptr, FALSE);
      DeleteObject(old);
    }
    shown.insert(id);
    if (!(GetWindowLongPtrW(p.H(id), GWL_STYLE) & WS_VISIBLE)) ShowWindow(p.H(id), SW_SHOWNA);
  };
  auto row = [&](unsigned id, unsigned label, float top, bool combo = false) {
    put(label, 16, top + 5, 210, 24);
    const bool number =
        id == 6012 || id == 6030 || id == 6031 || (id >= 6033 && id <= 6035);
    put(id, 232, top, number ? 96.f : w - 248, combo ? 150.f : 29.f);
  };
  if (connector) {
    auto field = [&](unsigned id, unsigned label, float top, bool combo = false) {
      SetWindowLongPtrW(p.H(label), GWL_STYLE, (GetWindowLongPtrW(p.H(label), GWL_STYLE) & ~SS_TYPEMASK) | SS_RIGHT);
      put(label, 12, top + 5, 76, 22); put(id, 96, top, w - 108, combo ? 180.f : 29.f);
    };
    if (subpage == 0) {
      for (unsigned i = 0; i < 3; ++i) put(6060 + i, i * (w + 8) / 3, 0, (w - 16) / 3, 68);
      p.panel = {x, y + 80, x + w, y + height};
      if (codex) {
        put(6161, 12, 94, 220, 24); put(6160, w - 128, 94, 112, 24);
        put(6110, 12, 122, w - 24, 28);
        put(6040, 12, 158, 156, 28);
        if (!p.login_url.empty() && p.probe && p.probe->login && !p.probe->cancelled) put(6065, 176, 158, 132, 28);
        else put(6009, 176, 158, 132, 28);
        put(6067, w - 124, 158, 112, 28);
        put(6099, 40, 196, w - 52, 36);
        p.spinner = {x + 12, y + 196, x + 32, y + 216};
        if (p.advanced) { field(6006, 6106, 246); field(6007, 6107, 282, true); }
        if (!p.auth_known && !p.probe && p.services.probe) p.BeginProbe(false);
      } else {
        put(6000, 12, 92, w - 306, 200);
        put(6001, w - 286, 92, 62, 28); put(6002, w - 216, 92, 124, 28); put(6003, w - 84, 92, 72, 28);
        field(6004, 6104, 128); field(6006, 6106, 162); field(6063, 6109, 196);
        field(6007, 6107, 230, true); field(6008, 6108, 264);
        put(6162, 96, 298, w - 108, 18);
        put(6041, 12, 324, 84, 28); put(6009, 104, 324, 164, 28);
        put(6042, 276, 324, 156, 28); put(6064, 440, 324, w - 452, 28);
        put(6099, 40, 362, w - 52, 32); p.spinner = {x + 12, y + 362, x + 32, y + 382};
        put(6164, 12, 398, w - 24, 22);
        p.route = {x + 12, y + 426, x + w - 12, y + 500};
        put(6165, 22, 434, w - 44, 20); put(6163, 22, 458, w - 44, 18);
        if (p.config.connections[p.selected].base_url.find("openrouter.ai") != std::string::npos) {
          put(6166, 22, 480, w - 44, 18);
        }
      }
    } else {
      put(6050, 0, 0, w, 36);
      p.panel = {x, y + 52, x + w, y + height};
      put(6150, 12, 64, w - 24, 40); put(6153, 12, 108, w - 24, 22);
      for (unsigned i = 0; i < 4; ++i) put(6051 + i, 12 + i * (w - 24) / 4, 140, (w - 24) / 4 - 2, 26);
      put(6151, 12, 176, w - 24, 30);
      put(6152, 12, 216, w - 160, 22); put(6056, w - 124, 211, 112, 28);
      put(6055, 12, 248, w - 24, 118);
      if (p.gateway_client == 1 || p.gateway_client == 2) {
        put(6154, 12, 382, w - 164, 22); put(6058, w - 148, 377, 136, 28);
        put(6057, 12, 414, w - 24, 44);
      }
      for (unsigned id = 6051; id <= 6058; ++id) EnableWindow(p.H(id), p.gateway_enabled);
    }
    p.RefreshControls();
    const float scale = static_cast<float>(dpi) / 96;
    const RECT scrollbar{static_cast<LONG>((body.right + 6) * scale),
                         static_cast<LONG>(body.top * scale),
                         static_cast<LONG>((body.right + 6) * scale) + MulDiv(12, static_cast<int>(dpi), 96),
                         static_cast<LONG>(body.bottom * scale)};
    ui::LayoutSettingsScrollbar(p.H(6070), scrollbar, static_cast<int>(height),
                                 static_cast<int>(body.height()), p.scroll);
    if (p.scroll_max) shown.insert(6070);

  } else if (page == ui::SettingsPage::kChord) {
    if (subpage == 0) {
      row(6010, 6111, 0);
      row(6011, 6112, 44, true);
      row(6012, 6113, 88);
      row(6014, 6114, 132);
      row(6015, 6115, 176);
      put(6016, 16, 226, 130, 32);
      put(6017, 154, 226, 130, 32);
      put(6013, 292, 226, 150, 32);
      put(6116, 16, 276, w - 32, 58);
      put(6099, 16, 340, w - 32, 45);
    } else {
      put(6020, 16, 0, w - 32, 155);
      put(6121, 16, 170, 180, 24);
      put(6122, 210, 170, 180, 24);
      put(6021, 16, 198, 178, 29);
      put(6022, 210, 198, 178, 29);
      put(6023, 404, 198, w - 420, 130);
      put(6024, 16, 240, 160, 30);
      put(6025, 186, 240, 120, 30);
      put(6126, 16, 282, w - 32, 24);
      put(6026, 16, 314, 180, 29);
      put(6027, 210, 314, 180, 30);
      put(6099, 16, 355, w - 32, 38);
    }
  } else if (page == ui::SettingsPage::kAppearance) {
    if (subpage == 1) {
      row(6033, 6133, 146);
      row(6034, 6134, 190);
      row(6035, 6135, 234);
      row(6030, 6130, 278);
    } else
      row(6032, 6132, 292, true);
  } else if (page == ui::SettingsPage::kBuffer && subpage == 0)
    row(6031, 6131, 270);
  // Retained controls stay visible and keep focus/selection during a relayout.
  // Only controls absent from the new page need a visibility transition.
  for (auto [id, h] : p.controls)
    if (!shown.contains(id) && (GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE))
      ShowWindow(h, SW_HIDE);
}
void ParitySettings::PaintFields(HDC dc, ui::ThemeId theme,
                                 unsigned dpi) const {
  const auto &p = *impl_;
  const auto &palette = ui::Palette(theme);
  const int saved = SaveDC(dc);
  auto to_pixels = [dpi](const ui::DipRect& box) { return RECT{static_cast<LONG>(box.left * dpi / 96), static_cast<LONG>(box.top * dpi / 96), static_cast<LONG>(box.right * dpi / 96), static_cast<LONG>(box.bottom * dpi / 96)}; };
  if (p.page == ui::SettingsPage::kApi) {
    const auto clip = to_pixels(p.viewport); IntersectClipRect(dc, clip.left, clip.top, clip.right, clip.bottom);
    const bool dim = p.subpage == 1 && !p.gateway_enabled;
    ui::FillRoundRect(dc, to_pixels(p.panel), MulDiv(10, dpi, 96), ui::ToColorRef(dim ? ui::Blend(palette.settings_background, palette.surface_secondary, .55f) : palette.surface_secondary));
    ui::StrokeRoundRect(dc, to_pixels(p.panel), MulDiv(10, dpi, 96), ui::ToColorRef(palette.border));
    if (p.route.width() > 0) ui::FillRoundRect(dc, to_pixels(p.route), MulDiv(8, dpi, 96), ui::ToColorRef(palette.surface_tertiary));
  }
  const float scale = static_cast<float>(dpi) / 96;
  for (const auto &box : impl_->edit_frames) {
    const RECT pixels{static_cast<LONG>(box.left * scale),
                      static_cast<LONG>(box.top * scale),
                      static_cast<LONG>(box.right * scale),
                      static_cast<LONG>(box.bottom * scale)};
    ui::FillRoundRect(dc, pixels, MulDiv(8, static_cast<int>(dpi), 96),
                      ui::ToColorRef(palette.surface));
    ui::StrokeRoundRect(dc, pixels, MulDiv(8, static_cast<int>(dpi), 96),
                        ui::ToColorRef(palette.border));
  }
  if (p.page == ui::SettingsPage::kApi && p.subpage == 0 && p.probe && p.spinner.width() > 0) {
    const auto box = to_pixels(p.spinner);
    const float angle = static_cast<float>(GetTickCount64() % 1000) * .0062831853f;
    const float c = std::cos(angle), sn = std::sin(angle), cx = (box.left + box.right) * .5f, cy = (box.top + box.bottom) * .5f;
    const int rotation = SaveDC(dc); SetGraphicsMode(dc, GM_ADVANCED);
    XFORM transform{c, sn, -sn, c, cx - c * cx + sn * cy, cy - sn * cx - c * cy};
    ModifyWorldTransform(dc, &transform, MWT_LEFTMULTIPLY);
    ui::DrawFluentIcon(dc, ui::FluentIcon::kSpinner, box, ui::ToColorRef(palette.accent_text));
    RestoreDC(dc, rotation);
  }
  RestoreDC(dc, saved);
}
bool ParitySettings::DrawControl(const DRAWITEMSTRUCT& draw, ui::ThemeId theme, unsigned dpi, HFONT font) const {
  if (draw.CtlType != ODT_BUTTON || (draw.CtlID != 6050 && (draw.CtlID < 6060 || draw.CtlID > 6062))) return false;
  const auto& p = *impl_; const auto& colors = ui::Palette(theme);
  const auto scale = [dpi](int x) { return MulDiv(x, dpi, 96); };
  const auto color = [&](std::uint32_t c) { return ui::ToColorRef(c); };
  const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
  const bool selected = GetPropW(draw.hwndItem, L"RimesSelectedSegment") != nullptr;
  ui::FillRectColor(draw.hDC, draw.rcItem, color(colors.settings_background));
  const auto old = SelectObject(draw.hDC, font); SetBkMode(draw.hDC, TRANSPARENT);
  RECT label = draw.rcItem;
  if (draw.CtlID == 6050) {
    label.left += scale(32); label.right -= scale(48);
    ui::DrawFluentIcon(draw.hDC, ui::FluentIcon::kConnector, {scale(2), scale(8), scale(22), scale(28)}, color(colors.text_secondary));
    SetTextColor(draw.hDC, color(colors.text_primary));
    DrawTextW(draw.hDC, L"启用本地网关", -1, &label, DT_SINGLELINE | DT_VCENTER);
    RECT track{draw.rcItem.right - scale(34), scale(10), draw.rcItem.right, scale(26)};
    ui::FillRoundRect(draw.hDC, track, scale(8), color(p.gateway_enabled ? colors.accent : colors.border_strong));
    RECT thumb{track.left + scale(p.gateway_enabled ? 20 : 2), track.top + scale(2), track.left + scale(p.gateway_enabled ? 32 : 14), track.bottom - scale(2)};
    ui::FillRoundRect(draw.hDC, thumb, scale(6), RGB(255,255,255));
  } else {
    ui::FillRoundRect(draw.hDC, draw.rcItem, scale(8), color(selected ? ui::Blend(colors.surface_secondary, colors.accent, .14f) : colors.surface_secondary));
    ui::StrokeRoundRect(draw.hDC, draw.rcItem, scale(8), color(selected ? colors.accent : colors.border));
    const auto ink = disabled ? colors.text_muted : colors.text_primary;
    const auto icon = draw.CtlID == 6062 ? ui::FluentIcon::kConnector : draw.CtlID == 6060 ? ui::FluentIcon::kChat : ui::FluentIcon::kSparkle;
    ui::DrawFluentIcon(draw.hDC, icon, {scale(12),scale(13),scale(32),scale(33)}, color(ink));
    ui::DrawFluentIcon(draw.hDC, selected ? ui::FluentIcon::kRadioOn : ui::FluentIcon::kRadioOff,
        {draw.rcItem.right-scale(24),scale(15),draw.rcItem.right-scale(10),scale(29)}, color(selected ? colors.accent_text : colors.text_muted));
    label = {scale(40),scale(10),draw.rcItem.right-scale(30),scale(36)};
    SetTextColor(draw.hDC, color(ink));
    DrawTextW(draw.hDC, draw.CtlID == 6060 ? L"ChatGPT" : draw.CtlID == 6061 ? L"Claude" : L"AI Provider", -1, &label, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    const auto detail_font = CreateFontW(-scale(10),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    SelectObject(draw.hDC, detail_font); SetTextColor(draw.hDC, color(colors.text_muted));
    label = {scale(12),scale(40),draw.rcItem.right-scale(12),scale(60)};
    DrawTextW(draw.hDC, draw.CtlID == 6060 ? L"浏览器授权 · 隔离运行" : draw.CtlID == 6061 ? L"Windows 暂未实现 · 已禁用" : L"多个 OpenAI 兼容 Provider", -1, &label, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SelectObject(draw.hDC, old); DeleteObject(detail_font);
  }
  SelectObject(draw.hDC, old);
  if (draw.itemState & ODS_FOCUS) { RECT focus = draw.rcItem; InflateRect(&focus, -scale(3), -scale(3)); DrawFocusRect(draw.hDC, &focus); }
  return true;
}
bool ParitySettings::Command(unsigned id, unsigned notification) {
  auto &p = *impl_;
  if (id < 6000 || id >= 6100 || p.syncing) return false;
  if (notification == BN_CLICKED && p.H(id) && !IsWindowEnabled(p.H(id))) return false;
  if ((id >= 6004 && id <= 6008 || id == 6063) &&
      (notification == EN_CHANGE || notification == CBN_EDITCHANGE || notification == CBN_SELCHANGE)) {
    if (id == 6006 && p.config.connections[p.selected].kind == "codex-cli") {
      p.auth_known = p.auth_ready = false;
      p.auth_identity = p.config.connections[p.selected].id + "|" + Utf8(Text(p.H(6006)));
      if (p.probe) p.probe->cancelled = true;
    }
    p.RefreshControls();
  }
  if ((id >= 6000 && id <= 6008 || id == 6063) &&
      (notification == EN_CHANGE || notification == CBN_EDITCHANGE ||
       notification == CBN_SELCHANGE))
    ++p.generation;
  try {
    if (id == 6000 && notification == CBN_SELCHANGE) {
      const auto item = SendMessageW(p.H(6000), CB_GETCURSEL, 0, 0);
      auto selected = SendMessageW(p.H(6000), CB_GETITEMDATA, item, 0);
      p.CaptureConnection();
      if (selected >= 0 &&
          static_cast<std::size_t>(selected) < p.config.connections.size())
        p.selected = static_cast<std::size_t>(selected);
      p.ShowConnection();
      p.Status(L"正在编辑此连接器；使用“设为当前文本模型”切换。");
    } else if ((id == 6060 || id == 6062) && notification == BN_CLICKED) {
      const bool codex = id == 6060;
      p.CaptureConnection(); ++p.generation;
      const std::string kind = codex ? "codex-cli" : "openai-compatible";
      auto found = std::find_if(p.config.connections.begin(), p.config.connections.end(),
          [&](const auto& c) { return c.kind == kind && (codex || c.id == p.last_api_id); });
      if (found == p.config.connections.end()) found = std::find_if(p.config.connections.begin(), p.config.connections.end(), [&](const auto& c) { return c.kind == kind; });
      if (found == p.config.connections.end()) {
        if (p.config.connections.size() >= 32) throw std::runtime_error("最多保存 32 个连接器。");
        p.config.connections.push_back({ID(), codex ? "Codex" : "新的 API", kind, "", "", "", ""});
        p.selected = p.config.connections.size() - 1;
      } else p.selected = static_cast<std::size_t>(found - p.config.connections.begin());
      SelectConnection(&p.config, p.config.connections[p.selected].id);
      p.scroll = 0; p.Status(L""); p.ShowConnection();
    } else if ((id == 6001 || id == 6002) && notification == BN_CLICKED) {
      ++p.generation;
      p.CaptureConnection();
      if (p.config.connections.size() >= 32)
        throw std::runtime_error("最多保存 32 个连接器。");
      p.config.connections.push_back(
          {ID(), id == 6002 ? "OpenRouter" : "新的 API", "openai-compatible",
           id == 6002 ? "https://openrouter.ai/api/v1" : "", "", "", id == 6002 ? "https://openrouter.ai/docs" : ""});
      p.selected = p.config.connections.size() - 1;
      p.ShowConnection();
    } else if (id == 6003 && notification == BN_CLICKED) {
      const auto removed = p.config.connections[p.selected];
      if (removed.kind == "codex-cli" || !p.services.confirm_delete || !p.services.confirm_delete(p.parent, Wide(removed.name))) return false;
      ++p.generation;
      p.config.connections.erase(p.config.connections.begin() + static_cast<std::ptrdiff_t>(p.selected));
      if (p.config.connections.empty()) p.config.connections.push_back({ID(), "Codex", "codex-cli", "", "", "", ""});
      p.selected = 0;
      if (std::none_of(p.config.connections.begin(), p.config.connections.end(), [&](const auto& c) { return c.id == p.config.active_connection; }))
        SelectConnection(&p.config, p.config.connections.front().id);
      for (std::size_t i = 0; i < p.config.connections.size(); ++i) if (p.config.connections[i].id == p.config.active_connection) p.selected = i;
      if (p.secrets.contains(removed.id)) { auto& key = p.secrets[removed.id]; if (!key.empty()) SecureZeroMemory(key.data(), key.size() * sizeof(wchar_t)); p.secrets.erase(removed.id); }
      p.Status(L"Provider 已移除。"); p.ShowConnection();
    } else if (id >= 6051 && id <= 6054 && notification == BN_CLICKED) {
      p.gateway_client = id - 6051; p.ShowGateway();
    } else if (id == 6050 && notification == BN_CLICKED) {
      p.gateway_enabled = !p.gateway_enabled;
      InvalidateRect(p.H(6050), nullptr, FALSE);
    } else if ((id == 6056 || id == 6058) && notification == BN_CLICKED) {
      auto text = GatewayConfiguration(p.gateway_client, true, id == 6058);
      const auto bytes = (text.size() + 1) * sizeof(wchar_t);
      HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
      void* data = memory ? GlobalLock(memory) : nullptr;
      if (!data) { if (memory) GlobalFree(memory); throw std::runtime_error("无法复制配置。"); }
      memcpy(data, text.c_str(), bytes); GlobalUnlock(memory);
      SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t));
      bool copied = false;
      if (OpenClipboard(p.parent)) {
        if (EmptyClipboard()) {
          // Mark before publishing text: clipboard history and the Capsule
          // collector must never archive a copied gateway credential.
          const auto marker = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
          if (marker && SetClipboardData(RegisterClipboardFormatW(
              L"ExcludeClipboardContentFromMonitorProcessing"), marker)) {
            if (SetClipboardData(CF_UNICODETEXT, memory)) copied = true;
          } else if (marker) GlobalFree(marker);
        }
        CloseClipboard();
      }
      if (!copied) { GlobalFree(memory); throw std::runtime_error("剪贴板暂时不可用。"); }
      p.Status(L"已复制配置。");
    } else if (id == 6041 && notification == BN_CLICKED) {
      const auto profile_id = p.config.connections[p.selected].id;
      auto& key = p.secrets[profile_id];
      if (!key.empty()) SecureZeroMemory(key.data(), key.size() * sizeof(wchar_t));
      key.clear(); Set(p.H(6008), L"");
      p.clear_secrets.insert(profile_id);
      p.secrets[profile_id] = L"";
      p.Status(L"已请求清除密钥。");
    } else if (id == 6042 && notification == BN_CLICKED) {
      if (!p.TextReady()) return false;
      p.CaptureConnection();
      SelectConnection(&p.config, p.config.connections[p.selected].id);
      p.Status(L"已选为当前文本模型。");
    } else if (id == 6040 && notification == BN_CLICKED) {
      if (p.probe && p.probe->login) {
        p.probe->cancelled = true; p.login_url.clear(); p.Status(L"正在取消登录…"); p.RefreshControls(); return true;
      }
      p.BeginProbe(true);
    } else if (id == 6009 && notification == BN_CLICKED) {
      p.BeginProbe(false);
    } else if (id == 6067 && notification == BN_CLICKED) {
      p.advanced = !p.advanced;
    } else if (id == 6064 && notification == BN_CLICKED) {
      const auto url = Text(p.H(6063));
      if (ValidConnectionURL(Utf8(url), true) && p.services.open_url)
        p.Status(p.services.open_url(url) ? L"已在浏览器打开文档。" : L"无法打开文档，请检查地址。", false);
    } else if (id == 6065 && notification == BN_CLICKED) {
      if (!p.login_url.empty() && p.probe && p.probe->login && !p.probe->cancelled && p.services.copy_private)
        p.Status(p.services.copy_private(Wide(p.login_url)) ? L"已复制登录链接，可在浏览器继续授权。" : L"复制失败，请重试。");
    } else if ((id == 6016 || id == 6017) && notification == BN_CLICKED)
      p.File(id == 6017);
    else if (id == 6013 && notification == BN_CLICKED) {
      p.profile = BuiltinChordProfile();
      p.ShowMappings();
      p.Status(L"已恢复 isaac2026。");
    } else if (id == 6020 && notification == LBN_SELCHANGE) {
      auto row = SendMessageW(p.H(6020), LB_GETCURSEL, 0, 0);
      if (row >= 0) {
        const auto &m =
            p.profile.at("mappings").at(static_cast<std::size_t>(row));
        Set(p.H(6021), Wide(m.at("keys")));
        Set(p.H(6022), Wide(m.at("output")));
        SendMessageW(p.H(6023), CB_SETCURSEL,
                     m.at("kind") == "syllable" ? 0 : 1, 0);
      }
    } else if (id == 6024 && notification == BN_CLICKED) {
      p.CaptureProfile();
      auto candidate = p.profile;
      const auto parsed = ChordProfile::Parse(candidate);
      const auto keys = Utf8(Text(p.H(6021)));
      auto &rows = candidate["mappings"];
      auto found = std::find_if(rows.begin(), rows.end(), [&](const auto &r) {
        return parsed.Canonical(r.at("keys")) == parsed.Canonical(keys);
      });
      core::Json row = {
          {"keys", keys},
          {"output", Utf8(Text(p.H(6022)))},
          {"kind", SendMessageW(p.H(6023), CB_GETCURSEL, 0, 0) == 0
                       ? "syllable"
                       : "fragment"}};
      if (found == rows.end())
        rows.push_back(row);
      else
        *found = row;
      candidate["id"] = candidate.value("id", "") == "builtin.flyyao"
                            ? ID()
                            : candidate.at("id").get<std::string>();
      (void)ChordProfile::Parse(candidate);
      p.profile = std::move(candidate);
      p.ShowMappings();
      p.Status(L"映射已更新。");
    } else if (id == 6025 && notification == BN_CLICKED) {
      auto row = SendMessageW(p.H(6020), LB_GETCURSEL, 0, 0);
      if (row >= 0 && p.profile["mappings"].size() > 1) {
        p.profile["mappings"].erase(static_cast<std::size_t>(row));
        p.ShowMappings();
      }
    } else if (id == 6027 && notification == BN_CLICKED) {
      p.CaptureProfile();
      const auto profile = ChordProfile::Parse(p.profile);
      const auto keys = Utf8(Text(p.H(6026)));
      const auto *mapping = profile.Find(keys);
      p.Status(mapping
                   ? Wide(profile.Canonical(keys) + " → " + mapping->output +
                          (mapping->kind == "syllable" ? " · 完整音节"
                                                       : " · 拼音片段"))
                   : L"此组合没有映射；检查左右键区或新增映射。");
    } else
      return false;
  } catch (const std::exception &error) {
    p.Status(Wide(error.what()), true);
  }
  return true;
}
bool ParitySettings::Tick() {
  auto &p = *impl_;
  const auto status = GatewayStatus();
  if (status != p.gateway_status) { p.gateway_status = status; Set(p.H(6153), status); }
  if (!p.probe) return false;
  auto state = p.probe;
  std::lock_guard lock(state->mutex);
  bool layout = false;
  if (state->login && !state->cancelled && !state->url.empty() && state->url != p.login_url &&
      CodexAuthorizationURL(state->url + "\n") == state->url) {
    p.login_url = state->url; p.Status(L"请在浏览器完成授权；也可复制登录链接继续。"); layout = true;
  }
  if (!state->done) {
    if (p.page == ui::SettingsPage::kApi && p.subpage == 0) {
      const auto& b = p.spinner;
      RECT rect{static_cast<LONG>(b.left * p.dpi / 96), static_cast<LONG>(b.top * p.dpi / 96), static_cast<LONG>(b.right * p.dpi / 96), static_cast<LONG>(b.bottom * p.dpi / 96)};
      InvalidateRect(p.parent, &rect, FALSE);
    }
    return layout;
  }
  KillTimer(p.parent, 13); p.login_url.clear();
  if (state->id == p.config.connections[p.selected].id && p.probe_generation == p.generation) {
    const auto current = Text(p.H(6007));
    SendMessageW(p.H(6007), CB_RESETCONTENT, 0, 0);
    for (const auto &model : state->result.models) { const auto name = Wide(model); SendMessageW(p.H(6007), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str())); }
    Set(p.H(6007), current);
    if (p.config.connections[p.selected].kind == "codex-cli") { p.auth_known = true; p.auth_ready = state->result.success; }
    p.Status(Wide(state->result.message), !state->result.success && !state->cancelled);
  } else if (state->identity == p.auth_identity && !state->cancelled) { p.auth_known = true; p.auth_ready = state->result.success; }
  p.probe.reset(); p.RefreshControls();
  return true;
}
bool ParitySettings::Scroll(UINT message, WPARAM value) {
  auto& p = *impl_; if (p.page != ui::SettingsPage::kApi || !p.scroll_max) return false;
  if (message == WM_MOUSEWHEEL) p.scroll -= static_cast<short>(HIWORD(value)) / WHEEL_DELTA * 48;
  else switch (LOWORD(value)) {
    case SB_LINEUP: p.scroll -= 24; break; case SB_LINEDOWN: p.scroll += 24; break;
    case SB_PAGEUP: p.scroll -= static_cast<int>(p.viewport.height()); break;
    case SB_PAGEDOWN: p.scroll += static_cast<int>(p.viewport.height()); break;
    case SB_TOP: p.scroll = 0; break; case SB_BOTTOM: p.scroll = p.scroll_max; break;
    case SB_THUMBTRACK: { SCROLLINFO info{sizeof(info), SIF_TRACKPOS}; GetScrollInfo(p.H(6070), SB_CTL, &info); p.scroll = info.nTrackPos; break; }
    default: return false;
  }
  p.scroll = (std::clamp)(p.scroll, 0, p.scroll_max); return true;
}
bool ParitySettings::Read(Settings *settings, std::string *error) {
  auto &p = *impl_;
  try {
    p.CaptureConnection();
    p.CaptureProfile();
    (void)ChordProfile::Parse(p.profile);
    settings->connections = p.config.connections;
    settings->gateway_enabled = p.gateway_enabled;
    if (!SelectConnection(settings, p.config.active_connection))
      return false;
    settings->buffer_width = Number(p.H(6030));
    settings->send_hold_ms = Number(p.H(6031));
    settings->follow_system_theme =
        SendMessageW(p.H(6032), CB_GETCURSEL, 0, 0) == 1;
    settings->candidate_width = Number(p.H(6033));
    settings->candidate_height = Number(p.H(6034));
    settings->candidate_label_size = Number(p.H(6035));
    settings->chord_enabled = SendMessageW(p.H(6011), CB_GETCURSEL, 0, 0) == 0;
    settings->chord_interval_ms = Number(p.H(6012));
    settings->chord_profile = p.profile;
    if (!ValidSettings(*settings))
      throw std::runtime_error(
          "请检查连接器名称、数字范围和键位配置；原设置保持不变。");
    return true;
  } catch (const std::exception &e) {
    if (error)
      *error = e.what();
    return false;
  }
}
const std::map<std::string, std::wstring> &ParitySettings::Secrets() const {
  return impl_->secrets;
}
void ParitySettings::Saved() {
  auto& p = *impl_;
  for (auto& [id, key] : p.secrets) {
    auto& prior = p.saved_secrets[id];
    if (!prior.empty()) SecureZeroMemory(prior.data(), prior.size() * sizeof(wchar_t));
    prior = key;
    const auto profile = std::find_if(p.config.connections.begin(), p.config.connections.end(), [&](const auto& c) { return c.id == id; });
    if (profile != p.config.connections.end()) p.stored_keys[id] = {profile->base_url, !key.empty()};
    if (!key.empty()) SecureZeroMemory(key.data(), key.size() * sizeof(wchar_t));
  }
  p.secrets.clear();
  p.clear_secrets.clear();
  p.RefreshControls();
  // Keep the masked editor and its caret intact while the user is typing.
  // Capturing an unchanged value does not rewrite that vault entry again.
}
std::vector<HWND> ParitySettings::VisibleControls() const {
  std::vector<HWND> result;
  for (const auto &[id, control] : impl_->controls)
    if ((GetWindowLongPtrW(control, GWL_STYLE) & (WS_VISIBLE | WS_TABSTOP)) ==
            (WS_VISIBLE | WS_TABSTOP) &&
        IsWindowEnabled(control))
      result.push_back(control);
  return result;
}
} // namespace rimes::windows::workbench
