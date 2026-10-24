#pragma once
#include <windows.h>
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../engine/rime_engine.hpp"
#include "../ui/theme.hpp"

namespace rimes::windows::workbench {
enum class UserLexiconKind { Chinese, Wubi86, English };
inline constexpr std::array<UserLexiconKind, 3> kUserLexicons = {
    UserLexiconKind::Chinese, UserLexiconKind::Wubi86, UserLexiconKind::English};
const char* UserLexiconDictionary(UserLexiconKind);
const wchar_t* UserLexiconName(UserLexiconKind);
enum class UserLexiconState { Unavailable, NoDatabase, DatabasePresent };
struct UserLexiconStatus {
  UserLexiconKind kind;
  UserLexiconState state = UserLexiconState::Unavailable;
  std::string message;
};
struct UserLexiconResult {
  UserLexiconKind kind;
  std::size_t entries = 0;
  std::filesystem::path file;
  bool self_describing = false;
  bool snapshot = false;
};
struct UserLexiconContents {
  std::string text, dictionary;
  std::size_t entries = 0;
  bool snapshot = false;
};
// Test boundary only; production construction binds to the existing RimeEngine.
struct UserLexiconBackend {
  std::function<bool(std::vector<std::string>*, std::string*)> dictionaries;
  std::function<int(const std::string&, const std::filesystem::path&, std::string*)> export_data, import_data;
  std::function<bool(const std::filesystem::path&, std::string*)> restore;
};
class UserLexiconService {
 public:
  inline static constexpr std::size_t kFileLimit = 64 * 1024 * 1024;
  UserLexiconService(engine::RimeEngine&, std::filesystem::path private_root);
  UserLexiconService(UserLexiconBackend, std::filesystem::path private_root);
  std::array<UserLexiconStatus, 3> Status();
  UserLexiconResult Import(UserLexiconKind, const std::filesystem::path&);
  UserLexiconResult Export(UserLexiconKind, const std::filesystem::path&);
  static UserLexiconContents Validate(UserLexiconKind, std::string);
 private:
  std::mutex mutex_;
  UserLexiconBackend backend_;
  std::filesystem::path root_;
};
void ShowUserLexicon(HWND owner, std::shared_ptr<UserLexiconService>, ui::ThemeId theme);
}  // namespace rimes::windows::workbench
