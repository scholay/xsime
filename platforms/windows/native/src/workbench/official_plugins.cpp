#include "official_plugins.hpp"
#include "official_features.hpp"
#include "official_plugin_catalog.generated.hpp"
#include "provider.hpp"
#include "rimes_version.hpp"
#include <Windows.h>
#include <bcrypt.h>
#include <objbase.h>
#include <winhttp.h>
#include <array>
#include <fstream>
#include <memory>
#include <sstream>

namespace rimes::windows::workbench {
using core::Json;
namespace {
constexpr std::size_t kLimit = 256 * 1024;
std::string Read(const std::filesystem::path& path, std::size_t limit) {
  if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > limit)
    throw std::runtime_error("Plugin file is unavailable");
  std::ifstream input(path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(input)), {});
  if (!input || bytes.size() > limit) throw std::runtime_error("Plugin file read failed");
  return bytes;
}
std::string Guid() {
  GUID value{};
  if (FAILED(CoCreateGuid(&value))) throw std::runtime_error("Plugin grant unavailable");
  wchar_t text[40]{}; StringFromGUID2(value, text, 40); return Utf8(text);
}
void Atomic(const std::filesystem::path& path, const std::string& bytes) {
  auto temporary = path; temporary += Wide("." + Guid() + ".tmp");
  try {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); output.close();
    if (!output || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("Plugin state cannot be saved");
  } catch (...) { std::error_code ignored; std::filesystem::remove(temporary, ignored); throw; }
}
std::string Hash(const std::string& bytes) {
  std::array<unsigned char, 32> digest{};
  if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                 reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                 static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size())) < 0)
    throw std::runtime_error("Plugin checksum unavailable");
  std::string text; constexpr char hex[] = "0123456789abcdef";
  for (const auto byte : digest) { text += hex[byte >> 4]; text += hex[byte & 15]; }
  return text;
}
std::array<unsigned,3> Version(const std::string& text) {
  std::array<unsigned,3> out{}; std::istringstream input(text); char first = 0, second = 0;
  if (!(input >> out[0] >> first >> out[1] >> second >> out[2]) || first != '.' || second != '.' || !input.eof())
    throw std::runtime_error("Invalid plugin version");
  return out;
}
struct Close { void operator()(void* handle) const { if (handle) WinHttpCloseHandle(handle); } };
using Internet = std::unique_ptr<void, Close>;
std::string Download(const std::string& source) {
  auto url = Wide(source);
  Internet session(WinHttpOpen(L"RIMES Plugin Installer/1.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session) throw std::runtime_error("Plugin download unavailable");
  WinHttpSetTimeouts(session.get(), 10000, 10000, 20000, 20000);
  for (int attempt = 0; attempt < 4; ++attempt) {
    URL_COMPONENTS parts{sizeof(parts)}; parts.dwHostNameLength = parts.dwUrlPathLength =
        parts.dwExtraInfoLength = parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts))
      throw std::runtime_error("Invalid plugin URL");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    if (parts.nScheme != INTERNET_SCHEME_HTTPS || parts.nPort != INTERNET_DEFAULT_HTTPS_PORT || parts.dwUserNameLength || parts.dwPasswordLength ||
        (host != L"github.com" && host != L"objects.githubusercontent.com" && host != L"release-assets.githubusercontent.com"))
      throw std::runtime_error("Invalid plugin download recipient");
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    Internet connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection) throw std::runtime_error("Plugin connection failed");
    Internet request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) throw std::runtime_error("Plugin connection failed");
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.get(), nullptr))
      throw std::runtime_error("Plugin download failed; please retry");
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr))
      throw std::runtime_error("Plugin download status unavailable");
    if (status >= 300 && status < 400) {
      std::array<wchar_t,8192> location{}; size = static_cast<DWORD>(location.size() * sizeof(wchar_t));
      if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_LOCATION, nullptr, location.data(), &size, nullptr))
        throw std::runtime_error("Plugin redirect unavailable");
      url = location.data(); continue;
    }
    if (status != 200) throw std::runtime_error("Plugin package is not available for download");
    std::string bytes; std::array<char,8192> buffer{}; DWORD count = 0;
    const auto started = GetTickCount64();
    for (;;) {
      if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count))
        throw std::runtime_error("Plugin download interrupted");
      if (!count) return bytes;
      if (bytes.size() + count > kLimit || GetTickCount64() - started > 60000)
        throw std::runtime_error("Plugin download exceeds size or time limit");
      bytes.append(buffer.data(), count);
    }
  }
  throw std::runtime_error("Too many plugin redirects");
}
void Error(std::string* error, const std::exception& value) { if (error) *error = value.what(); }
}  // namespace
OfficialPluginStore::OfficialPluginStore(std::filesystem::path root) : root_(std::move(root)) {
  catalog_ = Json::parse(official::kCatalogJSON);
  if (root_.empty()) {
    wchar_t local[32768]{}; const auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
    if (!size || size >= 32768) return;
    root_ = std::filesystem::path(local) / L"RIMES" / L"official-plugins-v1";
  }
  try {
    root_ = std::filesystem::absolute(root_).lexically_normal();
    auto migration = Path("migration.json");
    std::filesystem::create_directories(root_);
    if (!std::filesystem::exists(migration)) Atomic(migration, Json{
        {"legacy", std::filesystem::exists(root_.parent_path() / L"settings.json")},
        {"defaultsVersion", 2}}.dump());
    ready_ = true;
  } catch (...) { ready_ = false; }
}
std::filesystem::path OfficialPluginStore::Path(const std::string& name) const {
  if (root_.empty() || name.find_first_of("/\\:") != std::string::npos || name.find("..") != std::string::npos)
    throw std::runtime_error("Invalid plugin storage path");
  const auto path = root_ / Wide(name);
  for (const auto& p : {root_.parent_path(), root_, path}) {
    const auto attributes = GetFileAttributesW(p.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
      throw std::runtime_error("Plugin storage cannot use reparse points");
  }
  return path;
}
const Json& OfficialPluginStore::Entry(const std::string& id) const {
  for (const auto& entry : catalog_.at("plugins")) if (entry.at("id") == id) return entry;
  throw std::runtime_error("Unknown official plugin");
}
Json OfficialPluginStore::State(const std::string& id) {
  const auto& entry = Entry(id);
  if (!ready_) throw std::runtime_error("Plugin store unavailable");
  const auto file = Path(id + ".state.json");
  if (std::filesystem::exists(file)) {
    auto state = Json::parse(Read(file, 4096));
    if (state.at("sha256") != entry.at("sha256")) throw std::runtime_error("Plugin state belongs to another package");
    return state;
  }
  const auto migration = Json::parse(Read(Path("migration.json"), 4096));
  const auto legacy = migration.at("legacy").get<bool>();
  // Existing stores retain their original implicit defaults. Only a genuinely
  // new store starts ChatGPT uninstalled; explicit receipts always win above.
  const bool optional_codex = id == "builtin.codex-cli" && !legacy &&
      migration.value("defaultsVersion", 1) >= 2;
  const bool available = !optional_codex && (legacy || entry.at("platforms").at("windows").at("distribution") == "bundled");
  return {{"grant", "bundled-" + entry.at("sha256").get<std::string>()}, {"installed", available},
          {"enabled", available}, {"bundled", true}, {"sha256", entry.at("sha256")}};
}
std::string OfficialPluginStore::BundledData(const std::string& id) {
  for (const auto& entry : official::kPackages) if (entry.id == id) return std::string(entry.bytes);
  throw std::runtime_error("Bundled plugin unavailable");
}
Json OfficialPluginStore::Verified(const std::string& id, const std::string& bytes) const {
  const auto& entry = Entry(id);
  if (bytes.size() > kLimit || Hash(bytes) != entry.at("sha256").get<std::string>()) throw std::runtime_error("Plugin checksum mismatch");
  auto value = Json::parse(bytes);
  if (value.at("schemaVersion") != 2 || value.at("sdkVersion") != 1 || value.at("runtime") != "host-interpreted" ||
      value.at("id") != id || value.at("version") != entry.at("version") || value.at("capabilities") != entry.at("capabilities") ||
      !value.at("platforms").contains("windows") || Version(kProductVersion) < Version(value.at("minimumHostVersion").get<std::string>()))
    throw std::runtime_error("Incompatible plugin package");
  official::Validate(value); return value;
}
Json OfficialPluginStore::Package(const std::string& id) {
  std::lock_guard lock(mutex_); const auto state = State(id);
  if (!state.at("installed").get<bool>() || !state.at("enabled").get<bool>()) throw std::runtime_error("Install and enable this official plugin first");
  return Verified(id, state.at("bundled").get<bool>() ? BundledData(id) : Read(Path(id + ".json"), kLimit));
}
std::string OfficialPluginStore::Grant(const std::string& id) {
  std::lock_guard lock(mutex_);
  try { (void)Package(id); return State(id).at("grant").get<std::string>(); } catch (...) { return {}; }
}
// Selection is a preference, never a package grant or a saved Buffer draft.
std::string OfficialPluginStore::LoadBufferSelection() {
  std::lock_guard lock(mutex_);
  try {
    if (!ready_) return {};
    const auto value = Json::parse(Read(Path("buffer-selection.json"), 4096),
        [](int depth, Json::parse_event_t, Json&) {
          if (depth > 4) throw std::runtime_error("Invalid Buffer selection");
          return true;
        });
    if (value.at("version") != 1) return {};
    const auto id = value.at("plugin").get<std::string>();
    return id.empty() || Entry(id).at("kind") == "buffer" ? id : std::string{};
  } catch (...) { return {}; }
}
bool OfficialPluginStore::SaveBufferSelection(const std::string& id, std::string* error) {
  std::lock_guard lock(mutex_);
  try {
    if (!ready_ || (!id.empty() && (Entry(id).at("kind") != "buffer" || Grant(id).empty())))
      throw std::runtime_error("Buffer plugin is unavailable");
    Atomic(Path("buffer-selection.json"), Json{{"version", 1}, {"plugin", id}}.dump());
    return true;
  } catch (const std::exception& value) { Error(error, value); return false; }
}
std::vector<PluginView> OfficialPluginStore::Entries() {
  std::lock_guard lock(mutex_); std::vector<PluginView> result;
  for (const auto& entry : catalog_.at("plugins")) {
    PluginView view; view.id = entry.at("id"); view.name = entry.at("nameZH"); view.version = entry.at("version");
    view.kind = entry.at("kind");
    view.bundled = entry.at("platforms").at("windows").at("distribution") == "bundled";
    try { const auto state = State(view.id); view.installed = state.at("installed"); view.grant = state.at("grant"); view.enabled = !Grant(view.id).empty(); } catch (...) {}
    result.push_back(view);
  }
  return result;
}
void OfficialPluginStore::WriteState(const std::string& id, bool installed, bool enabled, bool bundled) {
  Atomic(Path(id + ".state.json"), Json{{"grant", Guid()}, {"installed", installed}, {"enabled", enabled},
      {"bundled", bundled}, {"sha256", Entry(id).at("sha256")}}.dump());
}
bool OfficialPluginStore::Enable(const std::string& id, bool enabled, std::string* error) {
  std::lock_guard lock(mutex_);
  try {
    const auto state = State(id); if (!state.at("installed").get<bool>()) throw std::runtime_error("Install the plugin first");
    const bool bundled = state.at("bundled"); (void)Verified(id, bundled ? BundledData(id) : Read(Path(id + ".json"), kLimit));
    WriteState(id, true, enabled, bundled); return true;
  } catch (const std::exception& value) { Error(error, value); return false; }
}
bool OfficialPluginStore::Uninstall(const std::string& id, std::string* error) {
  std::lock_guard lock(mutex_);
  try { (void)Entry(id); const auto file = Path(id + ".json"); WriteState(id, false, false, false); std::filesystem::remove(file); return true; }
  catch (const std::exception& value) { Error(error, value); return false; }
}
bool OfficialPluginStore::InstallData(const std::string& id, const std::string& bytes, const std::string& expected_grant, std::string* error) {
  std::lock_guard lock(mutex_);
  try {
    (void)Verified(id, bytes);
    if (State(id).at("grant") != expected_grant) throw std::runtime_error("Plugin state changed during download; please retry");
    Atomic(Path(id + ".json"), bytes); WriteState(id, true, false, false); return true;
  } catch (const std::exception& value) { Error(error, value); return false; }
}
bool OfficialPluginStore::Install(const std::string& id, std::string* error) {
  try {
    Json entry; std::string grant;
    {
      std::lock_guard lock(mutex_); entry = Entry(id);
      if (!ready_) throw std::runtime_error("Plugin store unavailable");
      try { grant = State(id).at("grant"); }
      catch (const std::exception&) {
        // Explicit install repairs corrupt/obsolete receipts, without granting
        // execution or allowing an older download to overwrite the repair.
        WriteState(id, false, false, false); grant = State(id).at("grant");
      }
    }
    const auto bytes = entry.at("platforms").at("windows").at("distribution") == "bundled" ? BundledData(id) : Download(entry.at("downloadURL"));
    return InstallData(id, bytes, grant, error);
  } catch (const std::exception& value) { Error(error, value); return false; }
}
}  // namespace rimes::windows::workbench
