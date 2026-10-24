#pragma once
#include <filesystem>
#include <optional>
#include <set>

#include "../workbench/provider.hpp"

namespace rimes::windows::peripherals {
using core::Json;
inline bool ClipboardHasFiles(const Json& value) {
  const auto archive = value.find("clipboard");
  if (archive == value.end() || !archive->is_object()) return false;
  const auto files = archive->find("files");
  return files != archive->end() && files->is_array() && !files->empty();
}
struct ClipboardContent;
struct ClipboardFormat;
enum class Module {
  Temporary,
  Capture,
  Notes,
  Resources,
  Passwords,
  Terminal,
  Chat,
  Inbox
};
inline constexpr const char* kModuleIds[] = {
    "temporary", "capture",  "notes", "resources",
    "passwords", "terminal", "chat",  "inbox"};
inline constexpr const wchar_t* kModuleNames[] = {
    L"临时", L"捕获", L"笔记", L"资源", L"密码", L"终端", L"对话", L"收件"};
std::string NewId();
std::filesystem::path DefaultRoot();
std::string ReadFile(const std::filesystem::path&,
                     std::size_t limit = 1024 * 1024);
void AtomicWrite(const std::filesystem::path&, const std::string&);
std::string Seal(const std::string&);
std::string Unseal(const std::string&);
void Wipe(std::string&);
struct CleanupPolicy {
  std::size_t recent_items = 200;
  std::uint64_t recent_bytes = 1024ULL * 1024 * 1024;
  std::uint64_t capture_bytes = 10ULL * 1024 * 1024 * 1024;
  std::uint64_t capture_age = 30ULL * 24 * 3600 * 10000000;
  std::uint64_t orphan_grace = 3600ULL * 10000000;
};
struct CleanupResult {
  std::size_t records = 0, assets = 0;
  std::uint64_t bytes = 0;
};

// One independently replaceable record per item. No document, password or
// conversation is part of the IME settings file or installation payload.
class Store {
 public:
  explicit Store(std::filesystem::path root = DefaultRoot());
  Json List(Module, const std::string& search = "",
            const std::string& filter = "all") const;
  Json Get(const std::string& id) const;
  Json Save(Json value, std::uint64_t expected_revision = 0);
  Json PromoteTemporary(const Json&);
  Json RecordClipboardText(const std::string&);
  Json RecordClipboard(const ClipboardContent&, Module = Module::Temporary);
  std::vector<ClipboardFormat> ClipboardFormats(const Json&) const;
  void Erase(const std::string& id, std::uint64_t expected_revision);
  Json Import(Module, const std::filesystem::path&,
              const std::string& terminal = "");
  Json ImportData(Module, const std::string& title,
                  const std::string& extension, const std::string& bytes,
                  const std::string& terminal = "", bool temporary_capture = false);
  CleanupResult Cleanup(const std::set<std::string>& protected_ids = {},
                        CleanupPolicy policy = {}, std::uint64_t now = 0);
  bool Enabled(Module) const;
  void Enable(Module, bool);
  std::optional<Module> CapsuleSelection() const;
  void RememberCapsuleSelection(Module);
  std::optional<Module> MailboxSelection() const;
  void RememberMailboxSelection(Module);
  bool CollectClipboard() const;
  void SetCollectClipboard(bool);
  Json Preferences() const;
  void Preferences(Json);
  std::filesystem::path Asset(const Json&) const;
  const std::filesystem::path& Root() const { return root_; }

 private:
  std::filesystem::path root_;
  std::filesystem::path RecordPath(const std::string&) const;
};

// Physical letter batches, never IME text. The public default is the same four
// chords as macOS; custom credentials persist only salted hashes.
class RevealGate {
 public:
  explicit RevealGate(const std::filesystem::path& root);
  bool Key(unsigned virtual_key, bool down);
  void Reset();
  bool Authorized() const;
  unsigned Progress() const { return static_cast<unsigned>(batches_.size()); }
  bool Held() const { return !held_.empty(); }
  bool Failed() const { return failed_; }
  void Change(const std::vector<std::string>& first,
              const std::vector<std::string>& confirm);

 private:
  std::filesystem::path root_;
  std::set<unsigned> held_, batch_;
  std::vector<std::string> batches_;
  std::uint64_t until_ = 0;
  bool failed_ = false;
};
Json FreezeRoute(const workbench::Settings&);
workbench::Settings ResolveRoute(const Json&, const workbench::Settings&);
std::string ConversationPrompt(const Json&);
}  // namespace rimes::windows::peripherals
