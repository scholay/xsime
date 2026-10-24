#include "user_lexicon.hpp"

#include <windows.h>
#include <objbase.h>
#include <sddl.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace rimes::windows::workbench {
namespace {
[[noreturn]] void Fail(const char* message) { throw std::runtime_error(message); }
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
void CheckPath(const std::filesystem::path& path) {
  if (!path.is_absolute() || path.native().find(L'\0') != std::wstring::npos)
    Fail("学习词库路径必须是有效绝对路径。");
  for (auto p = path; !p.empty() && p != p.parent_path(); p = p.parent_path()) {
    const auto attributes = GetFileAttributesW(p.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
      Fail("学习词库路径不能经过链接或重解析点。");
  }
}
struct Security {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, FALSE};
  Security() {
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)", SDDL_REVISION_1, &descriptor, nullptr))
      Fail("无法保护学习词库临时文件。");
    attributes.lpSecurityDescriptor = descriptor;
  }
  ~Security() { if (descriptor) LocalFree(descriptor); }
};
void PrivateDirectory(const std::filesystem::path& path) {
  CheckPath(path);
  std::filesystem::create_directories(path);
  CheckPath(path);
  Security security;
  if (!SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                        security.descriptor)) Fail("无法保护学习词库临时目录。");
}
std::wstring Unique() {
  GUID id{}; wchar_t value[40]{};
  if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, value, 40)) Fail("无法创建学习词库临时文件名。");
  return value;
}
void WriteNew(const std::filesystem::path& path, const std::string& text) {
  CheckPath(path);
  Security security;
  Handle file{CreateFileW(path.c_str(), GENERIC_WRITE, 0, &security.attributes, CREATE_NEW,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  if (file.value == INVALID_HANDLE_VALUE) Fail("无法创建学习词库文件。");
  DWORD written = 0;
  if (!WriteFile(file.value, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
      written != text.size() || !FlushFileBuffers(file.value)) Fail("写入学习词库文件失败。");
}
std::string Read(const std::filesystem::path& path) {
  CheckPath(path);
  // Deny concurrent writes/deletion while validating the selected file. The
  // library receives our private immutable copy, never this user-selected path.
  Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
  if (file.value == INVALID_HANDLE_VALUE) Fail("无法读取学习词库文件，文件可能不存在或正在使用。");
  BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER size{};
  if (GetFileType(file.value) != FILE_TYPE_DISK || !GetFileInformationByHandle(file.value, &info) ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
    Fail("学习词库必须是普通文件，不能是链接或目录。");
  if (!GetFileSizeEx(file.value, &size) || size.QuadPart < 0 ||
      size.QuadPart > UserLexiconService::kFileLimit) Fail("学习词库文件超过 64 MiB 安全上限。");
  std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
  DWORD read = 0;
  if (!ReadFile(file.value, text.data(), static_cast<DWORD>(text.size()), &read, nullptr) || read != text.size())
    Fail("读取学习词库文件失败。");
  return text;
}
void Destination(const std::filesystem::path& path) {
  CheckPath(path);
  if (!std::filesystem::is_directory(path.parent_path())) Fail("学习词库导出目录不存在。");
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES &&
      (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
    Fail("导出目标必须是普通文件，不能是目录或链接。");
}
struct Temporary {
  std::filesystem::path path;
  ~Temporary() { std::error_code ignored; std::filesystem::remove(path, ignored); }
};
void AtomicExport(const std::filesystem::path& path, const std::string& text) {
  Destination(path);
  Temporary staged{path.parent_path() / (L".xsime-learning-" + Unique() + L".tmp")};
  WriteNew(staged.path, text);
  Destination(path);
  if (!MoveFileExW(staged.path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    Fail("学习词库导出失败，原目标文件已保留。");
}
std::string_view Trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) value.remove_suffix(1);
  return value;
}
bool Weight(std::string_view text) {
  if (!text.empty() && text.front() == '+') text.remove_prefix(1);
  double value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size() && std::isfinite(value) && value >= 0;
}
bool Integer(std::string_view text, bool commit) {
  if (!text.empty() && text.front() == '+') text.remove_prefix(1);
  std::uint64_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
      value <= (commit ? static_cast<std::uint64_t>((std::numeric_limits<int>::max)())
                       : static_cast<std::uint64_t>((std::numeric_limits<unsigned long>::max)()));
}
bool Packed(std::string_view text) {
  std::set<std::string_view> seen;
  while (!text.empty()) {
    const auto space = text.find(' ');
    const auto field = text.substr(0, space);
    text = space == text.npos ? std::string_view{} : Trim(text.substr(space + 1));
    const auto equal = field.find('=');
    if (equal == field.npos || equal == 0 || equal + 1 == field.size()) return false;
    const auto key = field.substr(0, equal), value = field.substr(equal + 1);
    if (key == "c" || key == "d" || key == "t") {
      if (!seen.insert(key).second || (key == "d" ? !Weight(value) : !Integer(value, key == "c"))) return false;
    }
  }
  return seen.contains("c") && seen.contains("d") && seen.contains("t");
}
}  // namespace
const char* UserLexiconDictionary(UserLexiconKind kind) {
  switch (kind) {
    case UserLexiconKind::Chinese: return "rime_ice";
    case UserLexiconKind::Wubi86: return "wubi86";
    case UserLexiconKind::English: return "english";
  }
  Fail("未知学习词库。");
}
const wchar_t* UserLexiconName(UserLexiconKind kind) {
  switch (kind) {
    case UserLexiconKind::Chinese: return L"中文学习词库";
    case UserLexiconKind::Wubi86: return L"五笔86学习词库";
    case UserLexiconKind::English: return L"英文学习词库";
  }
  Fail("未知学习词库。");
}
UserLexiconService::UserLexiconService(engine::RimeEngine& engine, std::filesystem::path root)
    : UserLexiconService(UserLexiconBackend{
        [&engine](auto* names, auto* error) { return engine.UserDictionaries(names, error); },
        [&engine](const auto& name, const auto& path, auto* error) { return engine.ExportUserDictionary(name, path, error); },
        [&engine](const auto& name, const auto& path, auto* error) { return engine.ImportUserDictionary(name, path, error); },
        [&engine](const auto& path, auto* error) { return engine.RestoreUserDictionary(path, error); }}, std::move(root)) {}
UserLexiconService::UserLexiconService(UserLexiconBackend backend, std::filesystem::path root)
    : backend_(std::move(backend)), root_(std::filesystem::absolute(root).lexically_normal()) {}
std::array<UserLexiconStatus, 3> UserLexiconService::Status() {
  std::lock_guard lock(mutex_);
  std::vector<std::string> names;
  std::string error;
  const bool available = backend_.dictionaries(&names, &error);
  std::array<UserLexiconStatus, 3> result{};
  for (std::size_t i = 0; i < kUserLexicons.size(); ++i) {
    const auto kind = kUserLexicons[i];
    const bool present = std::find(names.begin(), names.end(), UserLexiconDictionary(kind)) != names.end();
    result[i] = {kind, !available ? UserLexiconState::Unavailable : present
        ? UserLexiconState::DatabasePresent : UserLexiconState::NoDatabase,
        !available ? (error.empty() ? "Rime 引擎尚未就绪。" : error)
        : present ? "已有学习数据库 · 不代表包含学习记录" : "尚未建立学习数据库"};
  }
  return result;
}
UserLexiconContents UserLexiconService::Validate(UserLexiconKind kind, std::string text) {
  if (text.size() > kFileLimit) Fail("学习词库文件超过 64 MiB 安全上限。");
  if (text.find('\0') != text.npos) Fail("学习词库含有无效的 NUL 字节。");
  if (!engine::detail::IsValidUtf8(text)) Fail("学习词库不是有效 UTF-8 文本。");
  if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
  UserLexiconContents result;
  std::string normalized;
  normalized.reserve(text.size());
  bool detected = false;
  std::size_t offset = 0, line_number = 0;
  while (offset <= text.size()) {
    const auto newline = text.find('\n', offset);
    auto line = std::string_view(text).substr(offset, newline == text.npos ? text.size() - offset : newline - offset);
    ++line_number;
    const auto malformed = [&] { throw std::runtime_error("学习词库第 " + std::to_string(line_number) + " 行不是 Rime 用户词典 TSV。"); };
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.size() > 64 * 1024) malformed();
    if (line.starts_with("#")) {
      constexpr std::string_view prefix = "#@/db_name";
      if (line.starts_with(prefix) && line.size() > prefix.size() &&
          (line[prefix.size()] == '\t' || line[prefix.size()] == ' ')) {
        const auto dictionary = Trim(line.substr(prefix.size()));
        if (dictionary.empty() || (!result.dictionary.empty() && result.dictionary != dictionary)) malformed();
        result.dictionary = dictionary;
        // The upstream TSV reader recognizes tab-separated metadata only.
        // Normalize the accepted whitespace form so it uses the validated ID.
        normalized += "#@/db_name\t";
        normalized += dictionary;
        normalized += '\n';
      } else if (line != "# no comment") {
        normalized += line; normalized += '\n';
      }
    } else if (!line.empty()) {
      const auto first = line.find('\t'), second = first == line.npos ? line.npos : line.find('\t', first + 1);
      if (second == line.npos || Trim(line.substr(0, first)).empty() || second == first + 1) malformed();
      auto value = line.substr(second + 1);
      value = Trim(value.substr(0, value.find('\t')));
      const bool portable = Weight(value);
      if (!portable && !Packed(value)) malformed();
      if (detected && result.snapshot == portable) malformed();
      detected = true; result.snapshot = !portable; ++result.entries;
      normalized += line; normalized += '\n';
    }
    if (newline == text.npos) break;
    offset = newline + 1;
  }
  if (!result.entries) Fail("学习词库中没有可处理的 Rime 学习记录。");
  if (!result.dictionary.empty() && result.dictionary != UserLexiconDictionary(kind))
    throw std::runtime_error("文件属于 " + result.dictionary + "，不能导入到 " + UserLexiconDictionary(kind) + "。");
  if (result.snapshot && result.dictionary.empty()) Fail("Rime 学习快照缺少 db_name，无法安全判断目标词库。");
  if (normalized.size() > kFileLimit) Fail("学习词库规范化后超过 64 MiB 安全上限。");
  result.text = std::move(normalized); return result;
}
UserLexiconResult UserLexiconService::Import(UserLexiconKind kind, const std::filesystem::path& source) {
  std::lock_guard lock(mutex_);
  auto content = Validate(kind, Read(source));
  PrivateDirectory(root_);
  // librime distinguishes its portable snapshot format by this exact suffix.
  Temporary temporary{root_ / (L".lexicon-" + Unique() + (content.snapshot ? L".userdb.txt" : L".tsv"))};
  WriteNew(temporary.path, content.text);
  // Keep a read-only lease open while librime reads, so even an accidental
  // writer in this process cannot replace the validated private snapshot.
  Handle immutable{CreateFileW(temporary.path.c_str(), GENERIC_READ, FILE_SHARE_READ,
      nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  if (immutable.value == INVALID_HANDLE_VALUE) Fail("无法锁定已验证的学习词库临时副本。");
  std::string error;
  const int count = content.snapshot ? (backend_.restore(temporary.path, &error) ? static_cast<int>(content.entries) : -1)
      : backend_.import_data(UserLexiconDictionary(kind), temporary.path, &error);
  if (count < 0) throw std::runtime_error(error.empty() ? "Rime 未能导入学习词库。" : error);
  return {kind, static_cast<std::size_t>(count), source, !content.dictionary.empty(), content.snapshot};
}
UserLexiconResult UserLexiconService::Export(UserLexiconKind kind, const std::filesystem::path& destination) {
  std::lock_guard lock(mutex_);
  Destination(destination); PrivateDirectory(root_);
  Temporary temporary{root_ / (L".lexicon-" + Unique() + L".tsv")};
  WriteNew(temporary.path, "");
  std::string error;
  const int count = backend_.export_data(UserLexiconDictionary(kind), temporary.path, &error);
  if (count < 0) throw std::runtime_error(error.empty() ? "Rime 未能导出学习词库。" : error);
  auto content = Validate(kind, Read(temporary.path));
  if (count != 0 && content.entries != static_cast<std::size_t>(count)) Fail("Rime 导出的学习条目数量不一致。");
  const auto annotated = "# RimeBuffer portable learning dictionary\n#@/rimebuffer_format\t1\n#@/db_name\t" +
      std::string(UserLexiconDictionary(kind)) + "\n" + content.text;
  if (annotated.size() > kFileLimit) Fail("学习词库导出文件超过 64 MiB 安全上限。");
  AtomicExport(destination, annotated);
  return {kind, content.entries, destination, true, content.snapshot};
}
}  // namespace rimes::windows::workbench
