#include "store.hpp"
#include "media.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <objbase.h>
#include <sddl.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <map>

namespace rimes::windows::peripherals {
using workbench::Utf8;
using workbench::Wide;
namespace {
constexpr std::size_t kTextLimit = 256 * 1024;
void CheckPath(const std::filesystem::path& path) {
  for (auto p = path; !p.empty() && p != p.parent_path(); p = p.parent_path()) {
    const auto a = GetFileAttributesW(p.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT))
      throw std::runtime_error("内容目录不能使用链接或重解析点。");
  }
}
void PrivateDirectory(const std::filesystem::path& path) {
  CheckPath(path);
  std::filesystem::create_directories(path);
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)", SDDL_REVISION_1, &sd,
          nullptr))
    throw std::runtime_error("无法设置内容目录权限。");
  const bool ok = SetFileSecurityW(path.c_str(),
                                   DACL_SECURITY_INFORMATION |
                                       PROTECTED_DACL_SECURITY_INFORMATION,
                                   sd) != FALSE;
  LocalFree(sd);
  if (!ok) throw std::runtime_error("无法保护本机内容目录。");
}
std::string Hex(const unsigned char* data, std::size_t size) {
  std::string result;
  result.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    result += "0123456789abcdef"[data[i] >> 4];
    result += "0123456789abcdef"[data[i] & 15];
  }
  return result;
}
std::string Unhex(const std::string& value) {
  if (value.size() % 2) throw std::runtime_error("加密数据损坏。");
  std::string result;
  for (std::size_t i = 0; i < value.size(); i += 2) {
    const auto a = std::string("0123456789abcdef").find(value[i]),
               b = std::string("0123456789abcdef").find(value[i + 1]);
    if (a > 15 || b > 15) throw std::runtime_error("加密数据损坏。");
    result += static_cast<char>((a << 4) | b);
  }
  return result;
}
std::string Digest(const std::string& value) {
  std::array<unsigned char, 32> digest{};
  if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                 reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
                 static_cast<ULONG>(value.size()), digest.data(),
                 static_cast<ULONG>(digest.size())) < 0)
    throw std::runtime_error("无法验证查看口令。");
  return Hex(digest.data(), digest.size());
}
std::string Canonical(const std::vector<std::string>& groups) {
  if (groups.size() != 4)
    throw std::runtime_error("查看口令必须是四组字母并击。");
  std::string result;
  for (auto s : groups) {
    if (s.empty() || s.size() > 26 ||
        s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos)
      throw std::runtime_error("查看口令只能包含物理字母键。");
    std::sort(s.begin(), s.end());
    s.erase(std::unique(s.begin(), s.end()), s.end());
    result += s + "/";
  }
  return result;
}
std::string Lower(std::string value) {
  for (char& c : value)
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return value;
}
std::uint64_t Now() {
  FILETIME t{};
  // The coarse system clock can give successive disk saves the same tick,
  // making the latest edit sort behind another note by its random identifier.
  GetSystemTimePreciseAsFileTime(&t);
  return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}
bool ValidId(const std::string& id) {
  return id.size() == 36 &&
         id.find_first_not_of("0123456789abcdef-") == std::string::npos;
}
bool FileName(const std::string& name) {
  return !name.empty() && name.size() <= 1024 && name != "." && name != ".." &&
      name.find_first_of("/\\:\"<>|?*") == std::string::npos && name.find('\0') == std::string::npos &&
      name.back() != '.' && name.back() != ' ';
}
}  // namespace
std::string NewId() {
  GUID id{};
  wchar_t text[40]{};
  if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, text, 40))
    throw std::runtime_error("无法创建内容身份。");
  return Lower(Utf8(text).substr(1, 36));
}
std::filesystem::path DefaultRoot() {
  wchar_t path[32768]{};
  const auto n = GetEnvironmentVariableW(L"LOCALAPPDATA", path, 32768);
  if (!n || n >= 32768) throw std::runtime_error("本机用户目录不可用。");
  return std::filesystem::path(path) / L"RIMES" / L"peripherals-v1";
}
std::string ReadFile(const std::filesystem::path& path, std::size_t limit) {
  CheckPath(path);
  if (!std::filesystem::is_regular_file(path))
    throw std::runtime_error("文件不可用或超过大小限制。");
  const auto size = std::filesystem::file_size(path);
  if (size > limit) throw std::runtime_error("文件不可用或超过大小限制。");
  std::ifstream in(path, std::ios::binary);
  std::string bytes(static_cast<std::size_t>(size), '\0');
  in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  // Never follow a concurrently growing source beyond the admission bound.
  if (!in || in.gcount() != static_cast<std::streamsize>(size) ||
      in.peek() != std::char_traits<char>::eof() || in.bad())
    throw std::runtime_error("读取文件失败。");
  return bytes;
}
void AtomicWrite(const std::filesystem::path& path, const std::string& bytes) {
  CheckPath(path);
  auto temp = path;
  temp += L"." + Wide(NewId()) + L".tmp";
  try {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out ||
        !MoveFileExW(temp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("保存失败，原内容已保留。");
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
    throw;
  }
}
void Wipe(std::string& value) {
  if (!value.empty()) SecureZeroMemory(value.data(), value.size());
  value.clear();
}
std::string Seal(const std::string& value) {
  DATA_BLOB input{static_cast<DWORD>(value.size()),
                  reinterpret_cast<BYTE*>(const_cast<char*>(value.data()))},
      output{};
  if (!CryptProtectData(&input, L"XSIME Capsule", nullptr, nullptr, nullptr,
                        CRYPTPROTECT_UI_FORBIDDEN, &output))
    throw std::runtime_error("密码加密失败。");
  auto text = Hex(output.pbData, output.cbData);
  LocalFree(output.pbData);
  return text;
}
std::string Unseal(const std::string& value) {
  auto bytes = Unhex(value);
  DATA_BLOB input{static_cast<DWORD>(bytes.size()),
                  reinterpret_cast<BYTE*>(bytes.data())},
      output{};
  if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output))
    throw std::runtime_error("无法由当前 Windows 用户解密。");
  std::string result(reinterpret_cast<char*>(output.pbData), output.cbData);
  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
  return result;
}
Store::Store(std::filesystem::path root)
    : root_(std::filesystem::absolute(root).lexically_normal()) {
  PrivateDirectory(root_);
  PrivateDirectory(root_ / L"records");
  PrivateDirectory(root_ / L"assets");
}
std::filesystem::path Store::RecordPath(const std::string& id) const {
  if (!ValidId(id)) throw std::runtime_error("内容身份无效。");
  auto path = root_ / L"records" / (Wide(id) + L".json");
  CheckPath(path);
  return path;
}
Json Store::Get(const std::string& id) const {
  auto value = Json::parse(ReadFile(RecordPath(id), 2 * 1024 * 1024));
  if (value.at("version") != 1 || value.at("id") != id)
    throw std::runtime_error("内容版本或身份不匹配。");
  return value;
}
Json Store::List(Module module, const std::string& search,
                 const std::string& filter) const {
  Json result = Json::array();
  const auto query = Lower(search);
  if (!Enabled(module)) return result;
  for (const auto& file :
       std::filesystem::directory_iterator(root_ / L"records")) {
    if (file.path().extension() != L".json") continue;
    try {
      auto value = Get(Utf8(file.path().stem().wstring()));
      if (value.at("module") != kModuleIds[static_cast<unsigned>(module)])
        continue;
      const bool mailbox = module == Module::Chat || module == Module::Inbox;
      if (mailbox && value.value("archived", false) != (filter == "archived"))
        continue;
      if (filter == "unread" && !value.value("unread", false)) continue;
      if (filter == "pending" && value.value("review", "pending") != "pending")
        continue;
      if (filter != "all" && filter != "archived" && filter != "unread" &&
          filter != "pending" && value.value("kind", "text") != filter)
        continue;
      auto searchable =
          value.value("title", "") + " " + value.value("summary", "");
      if (module != Module::Passwords)
        searchable += " " + value.value("body", "");
      if (!query.empty() && Lower(searchable).find(query) == std::string::npos)
        continue;
      result.push_back(std::move(value));
    } catch (
        ...) { /* One damaged record never hides the rest of the library. */
    }
  }
  std::sort(result.begin(), result.end(), [module](const Json& a, const Json& b) {
    const auto updated_a = a.value("updated", 0ULL), updated_b = b.value("updated", 0ULL);
    const auto order_a = module == Module::Temporary ? a.value("promoted", updated_a) : updated_a;
    const auto order_b = module == Module::Temporary ? b.value("promoted", updated_b) : updated_b;
    if (order_a != order_b) return order_a > order_b;
    if (updated_a != updated_b) return updated_a > updated_b;
    return a.at("id").get<std::string>() < b.at("id").get<std::string>();
  });
  return result;
}
Json Store::PromoteTemporary(const Json& item) {
  auto value = Get(item.at("id"));
  if (value.at("module") != "temporary" || value.at("revision") != item.at("revision"))
    throw std::runtime_error("历史条目已变化，请重新打开。");
  auto order = Now();
  const auto history = List(Module::Temporary);
  if (!history.empty())
    order = (std::max)(order, history.front().value("promoted",
        history.front().value("updated", 0ULL)) + 1);
  value["promoted"] = order;
  value["revision"] = value.at("revision").get<std::uint64_t>() + 1;
  AtomicWrite(RecordPath(value.at("id")), value.dump(2));
  return value;
}
Json Store::RecordClipboardText(const std::string& text) {
  if (text.empty() || text.size() > kTextLimit)
    throw std::runtime_error("剪贴板文字为空或过大。");
  for (const auto& item : List(Module::Temporary))
    if (item.value("kind", "") == "text" && !item.contains("asset") && !item.contains("clipboard") &&
        item.value("body", "") == text)
      return PromoteTemporary(item);
  return PromoteTemporary(Save({{"module", "temporary"}, {"kind", "text"},
                                 {"title", "剪贴板文字"}, {"body", text}}));
}
Json Store::RecordClipboard(const ClipboardContent& content, Module module) {
  if (module == Module::Passwords || module == Module::Chat || module == Module::Terminal ||
      module == Module::Resources) throw std::runtime_error("此模块不接受剪贴板归档。");
  // Keep legacy callers/records compatible. New OS snapshots always carry the
  // representations even when the primary preview happens to be plain text.
  if (content.formats.empty() && content.files.empty()) {
    if (!content.bytes.empty()) return ImportData(module, "剪贴板图片", content.extension, content.bytes);
    if (content.text.empty()) throw std::runtime_error("剪贴板中没有可导入的内容。");
    return module == Module::Temporary ? RecordClipboardText(content.text)
        : Save({{"module", kModuleIds[static_cast<unsigned>(module)]}, {"kind", "text"},
                {"title", "剪贴板文字"}, {"body", content.text}});
  }
  auto formats = content.formats;
  if (!formats.empty()) ValidateClipboardFormats(formats);
  std::sort(formats.begin(), formats.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
  std::size_t total = 0;
  Json identity = Json::array();
  for (const auto& format : formats) {
    if (format.name == "CF_HDROP") throw std::runtime_error("文件归档必须使用托管文件列表。");
    total += format.bytes.size();
    identity.push_back({format.name, Digest(format.bytes)});
  }
  if (content.files.size() > kClipboardFileLimit) throw std::runtime_error("一次最多收录 16 个文件。");
  struct File { std::string name, bytes; };
  std::vector<File> files;
  for (const auto& path : content.files) {
    auto name = Utf8(path.filename().wstring());
    if (!FileName(name)) throw std::runtime_error("剪贴板文件名无效。");
    auto bytes = ReadFile(path, kClipboardArchiveLimit - total);
    total += bytes.size();
    identity.push_back({"file", name, Digest(bytes)});
    files.push_back({std::move(name), std::move(bytes)});
  }
  const auto fingerprint = Digest(identity.dump());
  if (module == Module::Temporary)
    for (const auto& item : List(module))
      if (item.contains("clipboard") && item.value("fingerprint", "") == fingerprint)
        return PromoteTemporary(item);

  Json archive{{"version", 1}, {"formats", Json::array()}, {"files", Json::array()}};
  Json value{{"module", kModuleIds[static_cast<unsigned>(module)]}, {"kind", "text"},
             {"title", content.text.empty() ? "剪贴板内容" : "剪贴板文字"}, {"fingerprint", fingerprint}};
  if (!content.text.empty()) {
    if (content.text.size() > kTextLimit) throw std::runtime_error("剪贴板文字超过 256 KiB。");
    value["body"] = content.text;
  }
  std::vector<std::filesystem::path> written, directories;
  const auto write = [&](const std::string& name, const std::string& bytes) {
    const auto path = Asset({{"asset", name}});
    AtomicWrite(path, bytes); written.push_back(path);
  };
  bool committed = false;
  try {
    for (const auto& format : formats) {
      const auto name = NewId() + (format.name == "PNG" ? ".png" : ".clipdata");
      write(name, format.bytes);
      archive["formats"].push_back({{"name", format.name}, {"asset", name},
                                     {"bytes", format.bytes.size()}, {"digest", Digest(format.bytes)}});
      if (format.name == "PNG" && content.extension == ".png") value["asset"] = name;
    }
    if (!content.bytes.empty()) {
      if (!value.contains("asset")) {
        if (content.bytes.size() > kClipboardArchiveLimit - total)
          throw std::runtime_error("剪贴板归档及预览超过 64 MiB。");
        if (content.extension != ".bmp" && content.extension != ".png")
          throw std::runtime_error("剪贴板图片预览无效。");
        const auto preview = NewId() + content.extension;
        write(preview, content.bytes); value["asset"] = preview;
      }
      value["kind"] = "image"; value["title"] = "剪贴板图片";
    }
    for (const auto& file : files) {
      // A private UUID directory retains each original leaf name, including
      // duplicates from different source directories, without source links.
      const auto directory = NewId();
      const auto parent = root_ / L"assets" / Wide(directory);
      PrivateDirectory(parent); directories.push_back(parent);
      const auto name = directory + "/" + file.name;
      write(name, file.bytes);
      archive["files"].push_back({{"name", file.name}, {"asset", name},
                                   {"bytes", file.bytes.size()}, {"digest", Digest(file.bytes)}});
      if (!value.contains("asset")) value["asset"] = name;
    }
    if (!files.empty()) {
      value["kind"] = "document";
      value["title"] = files.size() == 1 ? files.front().name
          : "剪贴板文件（" + std::to_string(files.size()) + " 项）";
      std::string summary;
      for (const auto& file : files) { if (!summary.empty()) summary += " · "; summary += file.name; }
      value["summary"] = summary;
    }
    value["clipboard"] = std::move(archive);
    value["unread"] = module == Module::Inbox; value["review"] = "pending";
    auto saved = Save(value);
    committed = true;
    return module == Module::Temporary ? PromoteTemporary(saved) : saved;
  } catch (...) {
    std::error_code ignored;
    if (!committed) {
      for (const auto& path : written) std::filesystem::remove(path, ignored);
      for (const auto& path : directories) std::filesystem::remove(path, ignored);
    }
    throw;
  }
}
std::vector<ClipboardFormat> Store::ClipboardFormats(const Json& value) const {
  const auto& archive = value.at("clipboard");
  if (archive.at("version") != 1 || !archive.at("formats").is_array() ||
      archive.at("formats").size() > 6 || !archive.at("files").is_array() ||
      archive.at("files").size() > kClipboardFileLimit)
    throw std::runtime_error("剪贴板归档版本或项目数量无效。");
  std::size_t total = 0;
  const auto read = [&](const Json& entry) {
    const auto bytes = ReadFile(Asset(entry), kClipboardArchiveLimit - total);
    if (entry.at("bytes") != bytes.size() || entry.at("digest") != Digest(bytes))
      throw std::runtime_error("剪贴板归档已损坏，原剪贴板已保留。");
    total += bytes.size(); return bytes;
  };
  std::vector<ClipboardFormat> formats;
  for (const auto& entry : archive.at("formats")) {
    const auto name = entry.at("name").get<std::string>();
    if (name == "CF_HDROP") throw std::runtime_error("剪贴板文件归档无效。");
    formats.push_back({name, read(entry)});
  }
  std::vector<std::filesystem::path> paths;
  for (const auto& entry : archive.at("files")) {
    const auto path = Asset(entry);
    if (Utf8(path.filename().wstring()) != entry.at("name").get<std::string>())
      throw std::runtime_error("剪贴板文件名与归档不匹配。");
    (void)read(entry); paths.push_back(path);
  }
  if (!paths.empty()) formats.push_back({"CF_HDROP", ClipboardFileDrop(paths)});
  ValidateClipboardFormats(formats);
  return formats;
}
Json Store::Save(Json value, std::uint64_t expected) {
  const auto module = value.at("module").get<std::string>();
  if (std::find(std::begin(kModuleIds), std::end(kModuleIds), module) ==
      std::end(kModuleIds))
    throw std::runtime_error("未知内容模块。");
  if (value.value("title", "").empty() ||
      value.value("title", "").size() > 1024 ||
      value.value("body", "").size() > kTextLimit ||
      value.dump().size() > 1800 * 1024)
    throw std::runtime_error("请填写标题，正文最多 256 KiB。");
  const auto id = value.value("id", NewId());
  const auto path = RecordPath(id);
  if (std::filesystem::exists(path)) {
    const auto previous = Get(id);
    if (!expected || previous.at("revision") != expected)
      throw std::runtime_error("内容已在其他位置修改，请重新载入后再保存。");
    if (ClipboardHasFiles(previous)) {
      // Editing a card's metadata is not replacing its attachment group.
      // All files, including those beyond the primary preview, remain owned.
      value["asset"] = previous.at("asset");
      value["kind"] = "document";
      value["clipboard"] = previous.at("clipboard");
      if (previous.value("body", "") != value.value("body", "")) {
        value.erase("fingerprint");
        auto& formats = value["clipboard"]["formats"];
        formats.erase(std::remove_if(formats.begin(), formats.end(), [](const Json& format) {
          const auto name = format.value("name", "");
          return name == "CF_UNICODETEXT" || name == "HTML Format" || name == "Rich Text Format";
        }), formats.end());
      }
    } else if (previous.value("asset", "") != value.value("asset", "") ||
        previous.value("body", "") != value.value("body", "")) {
      value.erase("fingerprint");
      value.erase("clipboard");  // Editing a preview must never replay stale rich content.
    }
  } else if (expected)
    throw std::runtime_error("内容已被删除。");
  if (module == "passwords" && value.contains("body")) {
    auto plain = value.at("body").get<std::string>();
    value["sealed"] = Seal(plain);
    value.erase("body");
    value.erase("summary");
    Wipe(plain);
  }
  value["version"] = 1;
  value["id"] = id;
  value["revision"] = expected + 1;
  value["updated"] = Now();
  AtomicWrite(path, value.dump(2));
  return value;
}
void Store::Erase(const std::string& id, std::uint64_t expected) {
  if (Get(id).at("revision") != expected)
    throw std::runtime_error("内容已变化，请重新载入。");
  if (!std::filesystem::remove(RecordPath(id)))
    throw std::runtime_error("删除失败。");
  // Assets may be referenced by another record; never delete a source file.
}
Json Store::Import(Module module, const std::filesystem::path& source,
                   const std::string& terminal) {
  CheckPath(source);
  if (module == Module::Resources) {
    if (!std::filesystem::exists(source))
      throw std::runtime_error("资源路径不可用。");
    const auto extension = Lower(Utf8(source.extension().wstring()));
    return Save({{"module", "resources"},
                 {"title", Utf8(source.filename().wstring())},
                 {"kind", std::filesystem::is_directory(source) ? "project"
                          : extension == ".pdf"                 ? "literature"
                                                                : "document"},
                 {"body", Utf8(std::filesystem::absolute(source).wstring())}});
  }
  return ImportData(module, Utf8(source.filename().wstring()),
                    Lower(Utf8(source.extension().wstring())),
                    ReadFile(source, 64 * 1024 * 1024), terminal);
}
Json Store::ImportData(Module module, const std::string& title,
                       const std::string& extension, const std::string& bytes,
                       const std::string& terminal, bool temporary_capture) {
  if (module == Module::Passwords || module == Module::Chat ||
      module == Module::Terminal)
    throw std::runtime_error("此模块不接受普通附件导入。");
  if (extension.size() > 12 ||
      extension.find_first_not_of(".abcdefghijklmnopqrstuvwxyz0123456789") !=
          std::string::npos ||
      bytes.size() > 64 * 1024 * 1024)
    throw std::runtime_error("附件类型或大小无效。");
  const bool image = extension == ".png" || extension == ".jpg" ||
                     extension == ".jpeg" || extension == ".gif" ||
                     extension == ".bmp";
  const bool video = extension == ".mp4" || extension == ".mov" ||
                     extension == ".webm" || extension == ".gif";
  Json value{{"module", kModuleIds[static_cast<unsigned>(module)]},
             {"title", title},
             {"kind", module == Module::Capture && video ? "video"
                      : image                            ? "image"
                      : extension == ".pdf"              ? "literature"
                                                         : "document"}};
  if (!image && !video && extension != ".pdf") {
    bool text = bytes.find('\0') == std::string::npos && bytes.size() <= kTextLimit;
    try { if (text) (void)Wide(bytes); } catch (...) { text = false; }
    if (!text && module != Module::Temporary)
      throw std::runtime_error("只支持有界的 UTF-8 文本或媒体文件。");
    if (text) { value["body"] = bytes; value["kind"] = "text"; }
    else value["kind"] = "document";
  }
  if (module == Module::Temporary) {
    const auto fingerprint = Digest(extension + std::string(1, '\0') + bytes);
    for (const auto& item : List(module)) {
      bool same = item.value("fingerprint", "") == fingerprint;
      // Old records have no digest; compare matching assets without migrating
      // or rewriting any unrelated history entry.
      if (!same && !item.contains("fingerprint") && item.contains("asset")) {
        try {
          const auto path = Asset(item);
          same = path.extension() == Wide(extension) &&
                 std::filesystem::file_size(path) == bytes.size() &&
                 ReadFile(path, 64 * 1024 * 1024) == bytes;
        } catch (...) {}
      }
      if (same) return PromoteTemporary(item);
    }
    value["fingerprint"] = fingerprint;
  }
  const auto asset = NewId() + extension;
  value["asset"] = asset;
  value["terminalSessionID"] = terminal;
  value["unread"] = module == Module::Inbox;
  if (module == Module::Capture && temporary_capture) {
    value["temporaryCapture"] = true;
    value["captured"] = Now();
  }
  value["review"] = "pending";
  AtomicWrite(root_ / L"assets" / Wide(asset), bytes);
  Json saved;
  try {
    saved = Save(value);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(root_ / L"assets" / Wide(asset), ignored);
    throw;
  }
  return module == Module::Temporary ? PromoteTemporary(saved) : saved;
}
CleanupResult Store::Cleanup(const std::set<std::string>& protected_ids,
                             CleanupPolicy policy, std::uint64_t now) {
  if (!now) now = Now();
  CheckPath(root_ / L"assets");
  CheckPath(root_ / L"records");
  struct AssetInfo { std::filesystem::path path; std::uint64_t bytes, modified; };
  std::map<std::string, AssetInfo> assets;
  std::map<std::string, std::string> asset_paths;
  std::vector<std::filesystem::path> asset_directories;
  // Only one level of UUID-named private file containers is managed. Do not
  // follow directory links, descend into arbitrary trees or touch source paths.
  const auto inspect = [&](const std::filesystem::directory_entry& file, const std::string& name) {
    CheckPath(file.path());
    if (!file.is_regular_file()) throw std::runtime_error("历史附件结构无效，暂缓自动清理。");
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(file.path().c_str(), GetFileExInfoStandard, &data))
      throw std::runtime_error("无法核验历史附件，暂缓自动清理。");
    const auto modified = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                          data.ftLastWriteTime.dwLowDateTime;
    assets.emplace(name, AssetInfo{file.path(), file.file_size(), modified});
    asset_paths.emplace(Lower(Utf8(file.path().wstring())), name);
    asset_paths.emplace(Lower(Utf8(file.path().generic_wstring())), name);
  };
  for (const auto& file : std::filesystem::directory_iterator(root_ / L"assets")) {
    CheckPath(file.path());
    const auto name = Utf8(file.path().filename().wstring());
    if (name.size() < 36 || !ValidId(name.substr(0, 36))) continue;
    if (file.is_directory()) {
      if (!ValidId(name)) throw std::runtime_error("历史附件目录无效，暂缓自动清理。");
      asset_directories.push_back(file.path());
      for (const auto& child : std::filesystem::directory_iterator(file.path())) {
        const auto leaf = Utf8(child.path().filename().wstring());
        if (!FileName(leaf)) throw std::runtime_error("历史附件文件名无效，暂缓自动清理。");
        inspect(child, name + "/" + leaf);
      }
    } else inspect(file, name);
  }
  struct Record { Json value; std::set<std::string> assets; std::uint64_t bytes; bool removed = false; };
  std::vector<Record> records;
  std::map<std::string, std::size_t> references;
  const auto collect = [&](const auto& self, const Json& value, std::set<std::string>& found) -> void {
    if (value.is_string()) {
      const auto text = value.get<std::string>();
      if (assets.contains(text)) found.insert(text);
      else if (const auto path = asset_paths.find(Lower(text)); path != asset_paths.end()) found.insert(path->second);
    } else if (value.is_structured()) for (const auto& child : value) self(self, child, found);
  };
  // Inspect every module, including disabled modules. Unknown/corrupt records
  // abort before deletion: their references cannot safely be reconstructed.
  for (const auto& file : std::filesystem::directory_iterator(root_ / L"records")) {
    if (file.path().extension() != L".json") continue;
    auto value = Get(Utf8(file.path().stem().wstring()));
    const auto module = value.at("module").get<std::string>();
    if (std::find(std::begin(kModuleIds), std::end(kModuleIds), module) == std::end(kModuleIds))
      throw std::runtime_error("未知内容模块，暂缓自动清理。");
    (void)value.at("revision").get<std::uint64_t>();
    (void)value.at("updated").get<std::uint64_t>();
    if (value.contains("promoted")) (void)value.at("promoted").get<std::uint64_t>();
    if (value.contains("captured")) (void)value.at("captured").get<std::uint64_t>();
    if (value.contains("temporaryCapture")) (void)value.at("temporaryCapture").get<bool>();
    Record record{value, {}, file.file_size()};
    collect(collect, value, record.assets);
    for (const auto& name : record.assets) { ++references[name]; record.bytes += assets.at(name).bytes; }
    records.push_back(std::move(record));
  }
  CleanupResult result;
  std::set<std::string> retired_assets;
  const auto remove = [&](Record& record) {
    Erase(record.value.at("id"), record.value.at("revision"));
    record.removed = true; ++result.records;
    for (const auto& name : record.assets) { --references[name]; retired_assets.insert(name); }
  };
  std::vector<Record*> recent, captures;
  for (auto& record : records) {
    if (record.value.at("module") == "temporary") recent.push_back(&record);
    else if (record.value.at("module") == "capture" && record.value.value("temporaryCapture", false))
      captures.push_back(&record);
  }
  const auto order = [](const Record* record, bool history) {
    const auto updated = record->value.value("updated", 0ULL);
    return history ? record->value.value("promoted", updated) : record->value.value("captured", updated);
  };
  const auto trim = [&](std::vector<Record*>& items, bool history) {
    std::sort(items.begin(), items.end(), [&](const auto* a, const auto* b) {
      if (order(a, history) != order(b, history)) return order(a, history) < order(b, history);
      return a->value.at("id").template get<std::string>() > b->value.at("id").template get<std::string>();
    });
    std::uint64_t bytes = 0;
    for (const auto* record : items) bytes += record->bytes;
    auto count = items.size();
    for (auto* record : items) {
      if (protected_ids.contains(record->value.at("id").get<std::string>())) continue;
      if (!history && std::any_of(record->assets.begin(), record->assets.end(),
                                 [&](const auto& name) { return references[name] > 1; })) continue;
      const auto age = order(record, history);
      const bool expired = !history && now > age && now - age > policy.capture_age;
      if (!expired && bytes <= (history ? policy.recent_bytes : policy.capture_bytes) &&
          (!history || count <= policy.recent_items)) continue;
      remove(*record); bytes -= record->bytes; --count;
    }
  };
  trim(recent, true); trim(captures, false);
  for (const auto& [name, asset] : assets) {
    if (references[name] != 0) continue;
    if (!retired_assets.contains(name) &&
        (now <= asset.modified || now - asset.modified < policy.orphan_grace)) continue;
    // Validated private assets only; never a resource path or source file.
    CheckPath(asset.path);
    std::error_code error;
    if (std::filesystem::remove(asset.path, error)) { ++result.assets; result.bytes += asset.bytes; }
  }
  for (const auto& directory : asset_directories) {
    CheckPath(directory);
    std::error_code ignored;
    if (std::filesystem::is_empty(directory)) std::filesystem::remove(directory, ignored);
  }
  return result;
}
std::filesystem::path Store::Asset(const Json& value) const {
  const auto name = value.value("asset", "");
  const auto slash = name.find('/');
  const bool valid = slash == std::string::npos ? FileName(name)
      : ValidId(name.substr(0, slash)) && FileName(name.substr(slash + 1));
  if (!valid)
    throw std::runtime_error("附件不可用。");
  auto path = root_ / L"assets" / Wide(name);
  CheckPath(path);
  return path;
}
Json Store::Preferences() const {
  auto path = root_ / L"preferences.json";
  return std::filesystem::exists(path) ? Json::parse(ReadFile(path, 16384))
                                       : Json::object();
}
void Store::Preferences(Json value) {
  if (value.dump().size() > 16384) throw std::runtime_error("配置过大。");
  AtomicWrite(root_ / L"preferences.json", value.dump(2));
}
bool Store::Enabled(Module module) const {
  return Preferences().value(kModuleIds[static_cast<unsigned>(module)], true);
}
void Store::Enable(Module module, bool enabled) {
  auto value = Preferences();
  value[kModuleIds[static_cast<unsigned>(module)]] = enabled;
  Preferences(value);
}
std::optional<Module> Store::CapsuleSelection() const {
  const auto prefs = Preferences();
  const auto saved = prefs.find("capsule_last_module");
  // macOS's Recent tab is the Temporary clipboard-history module. Only
  // enabled Capsule modules may be restored; Mailbox has its own entry point.
  for (unsigned i = 0; i < 5; ++i)
    if (saved != prefs.end() && *saved == kModuleIds[i] &&
        prefs.value(kModuleIds[i], true))
      return static_cast<Module>(i);
  for (unsigned i = 0; i < 5; ++i)
    if (prefs.value(kModuleIds[i], true)) return static_cast<Module>(i);
  return std::nullopt;
}
void Store::RememberCapsuleSelection(Module module) {
  const auto index = static_cast<unsigned>(module);
  if (index >= 5) return;
  auto prefs = Preferences();
  if (!prefs.value(kModuleIds[index], true)) return;
  const auto saved = prefs.find("capsule_last_module");
  if (saved != prefs.end() && *saved == kModuleIds[index]) return;
  prefs["capsule_last_module"] = kModuleIds[index];
  Preferences(prefs);
}
std::optional<Module> Store::MailboxSelection() const {
  const auto prefs = Preferences();
  const auto saved = prefs.find("mailbox_last_module");
  for (unsigned i = 5; i < 8; ++i)
    if (saved != prefs.end() && *saved == kModuleIds[i] &&
        prefs.value(kModuleIds[i], true))
      return static_cast<Module>(i);
  // A new or unavailable selection opens Chat before the other workspaces.
  for (const auto module : {Module::Chat, Module::Terminal, Module::Inbox})
    if (prefs.value(kModuleIds[static_cast<unsigned>(module)], true)) return module;
  return std::nullopt;
}
void Store::RememberMailboxSelection(Module module) {
  const auto index = static_cast<unsigned>(module);
  if (index < 5 || index >= 8) return;
  auto prefs = Preferences();
  if (!prefs.value(kModuleIds[index], true)) return;
  const auto saved = prefs.find("mailbox_last_module");
  if (saved != prefs.end() && *saved == kModuleIds[index]) return;
  prefs["mailbox_last_module"] = kModuleIds[index];
  Preferences(prefs);
}
bool Store::CollectClipboard() const {
  // Match macOS's first-run default; an explicit pause remains persistent.
  return Preferences().value("clipboard", true);
}
void Store::SetCollectClipboard(bool enabled) {
  auto value = Preferences();
  value["clipboard"] = enabled;
  Preferences(value);
}
RevealGate::RevealGate(const std::filesystem::path& root) : root_(root) {}
void RevealGate::Reset() {
  held_.clear();
  batch_.clear();
  batches_.clear();
  until_ = 0;
  failed_ = false;
}
bool RevealGate::Authorized() const {
  return until_ && GetTickCount64() < until_;
}
bool RevealGate::Key(unsigned key, bool down) {
  if (key < 'A' || key > 'Z') return false;
  if (down) {
    if (batches_.size() == 4) Reset();
    held_.insert(key);
    batch_.insert(key);
    return true;
  }
  if (!held_.erase(key) || !held_.empty()) return true;
  std::string group;
  for (auto k : batch_) group += static_cast<char>(k);
  batch_.clear();
  batches_.push_back(group);
  if (batches_.size() != 4) return true;
  try {
    auto input = Canonical(batches_);
    const auto path = root_ / L"reveal-credential.json";
    bool valid = false;
    if (std::filesystem::exists(path)) {
      const auto c = Json::parse(ReadFile(path, 4096));
      valid = c.at("version") == 1 &&
              Digest(c.at("salt").get<std::string>() + input) ==
                  c.at("hash").get<std::string>();
    } else
      valid = input == Canonical({"RH", "WO", "CVN", "QU"});
    Wipe(input);
    failed_ = !valid;
    until_ = valid ? GetTickCount64() + 60000 : 0;
  } catch (...) {
    failed_ = true;
    until_ = 0;
  }
  return true;
}
void RevealGate::Change(const std::vector<std::string>& first,
                        const std::vector<std::string>& confirm) {
  if (!Authorized()) throw std::runtime_error("请先验证当前查看口令。");
  auto input = Canonical(first);
  if (input != Canonical(confirm)) throw std::runtime_error("两次口令不一致。");
  std::array<unsigned char, 24> salt{};
  if (BCryptGenRandom(nullptr, salt.data(), static_cast<ULONG>(salt.size()),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    throw std::runtime_error("随机源不可用。");
  auto encoded = Hex(salt.data(), salt.size());
  AtomicWrite(
      root_ / L"reveal-credential.json",
      Json{{"version", 1}, {"salt", encoded}, {"hash", Digest(encoded + input)}}
          .dump());
  Wipe(input);
  Reset();
}
Json FreezeRoute(const workbench::Settings& s) {
  return {{"id", s.active_connection},
          {"kind", s.ai_connector},
          {"url", s.ai_connector == "codex-cli" ? "" : s.base_url},
          {"model", s.ai_connector == "codex-cli" ? s.codex_model : s.model},
          {"executable", s.ai_connector == "codex-cli" ? s.codex_path : ""}};
}
workbench::Settings ResolveRoute(const Json& route,
                                 const workbench::Settings& current) {
  auto s = current;
  if (!workbench::SelectConnection(&s, route.at("id")))
    throw std::runtime_error("此对话的连接器已移除，请恢复连接器或新建对话。");
  if (FreezeRoute(s) != route)
    throw std::runtime_error(
        "此对话的连接器或模型已变化，请恢复原配置或新建对话。");
  return s;
}
std::string ConversationPrompt(const Json& conversation) {
  Json messages = conversation.value("messages", Json::array());
  const auto text = messages.dump();
  if (text.size() > kTextLimit)
    throw std::runtime_error("此对话达到上下文上限，请新建对话。");
  return text;
}
}  // namespace rimes::windows::peripherals
