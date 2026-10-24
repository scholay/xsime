#include "rime_engine.hpp"

#ifndef _WIN32
#error "RimeEngine is a Windows broker component"
#endif

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <functional>
#include <limits>
#include <mutex>
#include <set>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "rime_abi.hpp"
#include "candidate_paging.hpp"

namespace rimes::windows::engine {
namespace {

inline constexpr std::size_t kMaximumWindowsPath = 32767;
inline constexpr std::size_t kMaximumSelectKeysBytes =
    core::kMaxCandidateCount * 4;
inline constexpr std::size_t kMaximumApiBytes = 64U * 1024U;

std::atomic<void*> g_runtime_owner = nullptr;

void SetError(std::string* error, std::string_view message) noexcept {
  if (error == nullptr) {
    return;
  }
  try {
    error->assign(message);
  } catch (...) {
    // Diagnostics are best effort at this boundary.
  }
}

std::string Win32ErrorMessage(DWORD code) noexcept {
  try {
    wchar_t* system_message = nullptr;
    const DWORD count = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&system_message), 0,
        nullptr);
    if (count == 0 || system_message == nullptr) {
      return "Win32 error " + std::to_string(code);
    }
    struct LocalMemoryGuard {
      HLOCAL memory;
      ~LocalMemoryGuard() { LocalFree(memory); }
    };
    [[maybe_unused]] LocalMemoryGuard message_guard{system_message};

    std::wstring_view wide(system_message, count);
    while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' ||
                             wide.back() == L' ')) {
      wide.remove_suffix(1);
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string result;
    if (required > 0) {
      result.resize(static_cast<std::size_t>(required));
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                          static_cast<int>(wide.size()), result.data(),
                          required, nullptr, nullptr);
    } else {
      result = "Win32 error " + std::to_string(code);
    }
    return result;
  } catch (...) {
    return "Win32 operation failed";
  }
}

bool WideToUtf8(std::wstring_view wide, std::string* output,
                std::string* error) noexcept {
  try {
    if (wide.empty()) {
      output->clear();
      return true;
    }
    if (wide.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      SetError(error, "path is too long to encode as UTF-8");
      return false;
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
      SetError(error, "path contains invalid Unicode");
      return false;
    }
    std::string encoded(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), encoded.data(),
                            required, nullptr, nullptr) != required) {
      SetError(error, "failed to encode path as UTF-8");
      return false;
    }
    *output = std::move(encoded);
    return true;
  } catch (...) {
    SetError(error, "exception while encoding a path");
    return false;
  }
}

bool NormalizeDllPath(const std::filesystem::path& path, std::wstring* output,
                      std::string* error) noexcept {
  try {
    if (path.empty() || !path.is_absolute()) {
      SetError(error, "rime.dll path must be absolute");
      return false;
    }
    const std::wstring input = path.native();
    if (input.size() > kMaximumWindowsPath) {
      SetError(error, "rime.dll path exceeds the Windows path limit");
      return false;
    }

    const DWORD required = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (required == 0 || required > kMaximumWindowsPath) {
      SetError(error, "failed to normalize the rime.dll path");
      return false;
    }
    std::wstring normalized(required, L'\0');
    const DWORD written =
        GetFullPathNameW(input.c_str(), required, normalized.data(), nullptr);
    if (written == 0 || written >= required) {
      SetError(error, "failed to normalize the rime.dll path");
      return false;
    }
    normalized.resize(written);

    // Do not load executable code from UNC shares or device namespaces. The
    // broker accepts an ordinary absolute path on a local fixed drive only.
    if (normalized.size() < 3 ||
        !std::iswalpha(static_cast<wint_t>(normalized[0])) ||
        normalized[1] != L':' ||
        (normalized[2] != L'\\' && normalized[2] != L'/')) {
      SetError(error, "rime.dll must reside on a local drive");
      return false;
    }
    wchar_t root[] = {normalized[0], L':', L'\\', L'\0'};
    if (GetDriveTypeW(root) != DRIVE_FIXED) {
      SetError(error, "rime.dll must reside on a fixed local drive");
      return false;
    }

    const std::filesystem::path normalized_path(normalized);
    if (CompareStringOrdinal(normalized_path.filename().c_str(), -1,
                             L"rime.dll", -1, TRUE) != CSTR_EQUAL) {
      SetError(error, "engine DLL filename must be rime.dll");
      return false;
    }
    const DWORD attributes = GetFileAttributesW(normalized.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      SetError(error, "rime.dll path does not identify a file");
      return false;
    }

    *output = std::move(normalized);
    return true;
  } catch (...) {
    SetError(error, "exception while validating the rime.dll path");
    return false;
  }
}

bool CopyBoundedCString(const char* value, std::size_t maximum_bytes,
                        std::string* output, std::string* error,
                        const char* field, bool optional_comment = false) noexcept {
  try {
    if (value == nullptr) {
      output->clear();
      return true;
    }
    const void* terminator = std::memchr(value, 0, maximum_bytes + 1);
    if (terminator == nullptr) {
      SetError(error, "librime returned an unterminated or oversized string");
      return false;
    }
    const auto length =
        static_cast<std::size_t>(static_cast<const char*>(terminator) - value);
    output->assign(value, length);
    if (!detail::IsValidUtf8(*output)) {
      if (optional_comment) {
        // A Lua reverse-lookup annotation is display metadata. An invalid
        // comment must not disable otherwise valid composition or candidates.
        output->clear();
        return true;
      }
      SetError(error, std::string("librime returned invalid UTF-8 in ") + field);
      return false;
    }
    return true;
  } catch (...) {
    SetError(error, "exception while copying librime text");
    return false;
  }
}

std::vector<std::string_view> SplitUtf8Scalars(std::string_view text) {
  std::vector<std::string_view> scalars;
  scalars.reserve(core::kMaxCandidateCount);
  std::size_t index = 0;
  while (index < text.size() && scalars.size() < core::kMaxCandidateCount) {
    const auto first = static_cast<unsigned char>(text[index]);
    std::size_t width = 1;
    if (first >= 0xc2U && first <= 0xdfU) {
      width = 2;
    } else if (first >= 0xe0U && first <= 0xefU) {
      width = 3;
    } else if (first >= 0xf0U && first <= 0xf4U) {
      width = 4;
    }
    scalars.push_back(text.substr(index, width));
    index += width;
  }
  return scalars;
}

bool HasRequiredApi(const abi::ApiPrefix* api) noexcept {
  if (api == nullptr || api->data_size < 0) {
    return false;
  }
  const std::size_t available =
      sizeof(api->data_size) + static_cast<std::size_t>(api->data_size);
  constexpr auto required = offsetof(abi::ApiPrefix, config_set_int) + sizeof(abi::ApiPrefix::config_set_int);
  if (available < required || available > kMaximumApiBytes) {
    return false;
  }
  return api->setup != nullptr && api->initialize != nullptr &&
         api->finalize != nullptr && api->start_maintenance != nullptr &&
         api->is_maintenance_mode != nullptr &&
         api->join_maintenance_thread != nullptr &&
         api->create_session != nullptr && api->destroy_session != nullptr &&
         api->process_key != nullptr && api->get_commit != nullptr &&
         api->free_commit != nullptr && api->get_context != nullptr &&
         api->free_context != nullptr;
}

}  // namespace

class RimeEngine::Impl final {
 public:
  bool Start(const RimeEngineOptions& options, std::string* error) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      if (healthy_) {
        return true;
      }

      void* expected = nullptr;
      if (!g_runtime_owner.compare_exchange_strong(expected, this)) {
        SetError(error,
                 "another RimeEngine owns the process-wide librime runtime");
        return false;
      }
      owns_runtime_ = true;

      std::wstring dll_path;
      if (!NormalizeDllPath(options.dll_path, &dll_path, error) ||
          !StoreDataPaths(options, error)) {
        StopLocked();
        return false;
      }

      module_ = LoadLibraryExW(
          dll_path.c_str(), nullptr,
          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (module_ == nullptr) {
        const std::string win32_error = Win32ErrorMessage(GetLastError());
        SetError(error, "secure LoadLibraryExW failed: " + win32_error);
        StopLocked();
        return false;
      }

      const FARPROC get_api_symbol = GetProcAddress(module_, "rime_get_api");
      if (get_api_symbol == nullptr) {
        SetError(error, "rime.dll does not export rime_get_api");
        StopLocked();
        return false;
      }
      static_assert(sizeof(get_api_symbol) == sizeof(abi::GetApiFunction));
      abi::GetApiFunction get_api = nullptr;
      std::memcpy(&get_api, &get_api_symbol, sizeof(get_api));
      api_ = get_api();
      if (!HasRequiredApi(api_)) {
        SetError(error, "rime.dll exposes an incompatible RimeApi prefix");
        StopLocked();
        return false;
      }

      abi::Traits traits;
      abi::InitializeVersionedStruct(&traits);
      traits.shared_data_dir = shared_data_dir_.c_str();
      traits.user_data_dir = user_data_dir_.c_str();
      traits.distribution_name = "RIMES";
      traits.distribution_code_name = "rimes";
      traits.distribution_version = "0.1.0";
      traits.app_name = "rime.rimes.windows";
      static const char* import_modules[] = {"default", "lua", "deployer", nullptr};
      traits.modules = options.skip_maintenance ? import_modules : nullptr;
      traits.min_log_level = options.skip_maintenance ? 2 : 3;
      traits.log_dir = log_dir_.empty() ? nullptr : log_dir_.c_str();
      traits.prebuilt_data_dir = nullptr;
      traits.staging_dir = nullptr;

      api_->setup(&traits);
      api_->initialize(&traits);
      initialized_ = true;

      if (!options.skip_maintenance && !RunMaintenanceLocked(options.full_maintenance_check, error)) {
        StopLocked();
        return false;
      }

      if (options.verify_input_session) {
        const abi::SessionId smoke = api_->create_session();
        if (smoke == 0) {
          SetError(error, "librime smoke session creation failed");
          StopLocked();
          return false;
        }
        if (api_->destroy_session(smoke) == 0) {
          SetError(error, "librime smoke session destruction failed");
          StopLocked();
          return false;
        }
      }
      healthy_ = true;
      return true;
    } catch (...) {
      try {
        std::lock_guard<std::mutex> lock(mutex_);
        StopLocked();
      } catch (...) {
      }
      SetError(error, "exception while starting librime");
      return false;
    }
  }

  void Stop() noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      StopLocked();
    } catch (...) {
      // Destruction cannot surface an exception across the broker boundary.
    }
  }

  bool IsHealthy() const noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      return healthy_ && api_ != nullptr && module_ != nullptr;
    } catch (...) {
      return false;
    }
  }

  bool RunMaintenance(bool full_check, std::string* error) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!healthy_) {
        SetError(error, "librime is not initialized");
        return false;
      }
      if (!sessions_.empty()) {
        SetError(error,
                 "librime maintenance requires all input sessions to close");
        return false;
      }
      return RunMaintenanceLocked(full_check, error);
    } catch (...) {
      SetError(error, "exception while running librime maintenance");
      return false;
    }
  }

  SessionId CreateSession(std::string* error) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!healthy_ || api_ == nullptr) {
        SetError(error, "librime is not initialized");
        return 0;
      }
      const abi::SessionId session = api_->create_session();
      if (session == 0) {
        SetError(error, "librime session creation failed");
        return 0;
      }
      try {
        sessions_.insert(session);
      } catch (...) {
        api_->destroy_session(session);
        throw;
      }
      return session;
    } catch (...) {
      SetError(error, "exception while creating a librime session");
      return 0;
    }
  }

  bool DestroySession(SessionId session, std::string* error) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!healthy_ || api_ == nullptr || session == 0 ||
          sessions_.erase(session) != 1) {
        SetError(error, "unknown or inactive librime session");
        return false;
      }
      input_habits_.erase(session);
      candidate_page_keys_.erase(session);
      if (api_->destroy_session(session) == 0) {
        SetError(error, "librime rejected session destruction");
        return false;
      }
      return true;
    } catch (...) {
      SetError(error, "exception while destroying a librime session");
      return false;
    }
  }

  bool IsAsciiMode(SessionId session) const noexcept {
    try {
      std::lock_guard lock(mutex_);
      return healthy_ && api_ && api_->get_option && sessions_.contains(session) &&
             api_->get_option(session, "ascii_mode") != 0;
    } catch (...) { return false; }
  }

  bool ProcessChordText(SessionId session, const std::string& text, const std::string& replacement,
                        const EngineSnapshot& expected, EngineSnapshot* output,
                        bool* replaced, std::string* error) noexcept {
    try {
      std::lock_guard lock(mutex_);
      if (!output || !replaced) return false;
      *output = {}; *replaced = false;
      if (!healthy_ || !sessions_.contains(session) || !api_ || !api_->clear_composition) return false;
      EngineSnapshot current;
      if (!CollectSnapshotLocked(session, &current, error)) return false;
      std::string input = text;
      if (!replacement.empty() && current.commit_text.empty() && current.composing &&
          current.composition == expected.composition && current.caret_utf16 == expected.caret_utf16 &&
          current.selection_start_utf16 == expected.selection_start_utf16 &&
          current.selection_length_utf16 == expected.selection_length_utf16) {
        api_->clear_composition(session); input = replacement; *replaced = true;
      }
      std::string commits = current.commit_text;
      for (unsigned char key : input) {
        api_->process_key(session, key, 0);
        if (!CollectSnapshotLocked(session, &current, error)) return false;
        commits += current.commit_text;
      }
      current.commit_text = commits; current.handled = true;
      *output = std::move(current); return true;
    } catch (...) { SetError(error, "Chord processing failed"); return false; }
  }

  bool ProcessKey(SessionId session, std::int32_t keycode,
                  std::int32_t modifiers, EngineSnapshot* output,
                  std::string* error) noexcept {
    if (output != nullptr) {
      *output = EngineSnapshot{};
    }
    if (output == nullptr) {
      SetError(error, "engine snapshot output is null");
      return false;
    }

    try {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!healthy_ || api_ == nullptr || session == 0 ||
          !sessions_.contains(session)) {
        SetError(error, "unknown or inactive librime session");
        return false;
      }

      const bool handled = api_->process_key(session, keycode, modifiers) != 0;
      // ascii_composer records Shift down, and can commit raw code or toggle
      // ASCII on release while process_key still returns false. Collect only
      // this explicit modifier path; ordinary unhandled keys keep their
      // existing no-mutation/pass-through contract.
      const bool shift = keycode == 0xffe1 || keycode == 0xffe2;
      if (shift) {
        if (!CollectSnapshotLocked(session, output, error)) return false;
        output->handled = false;
        output->modifier_snapshot = true;
        return true;
      }
      if (!handled) {
        RawSnapshotView raw;
        raw.handled = false;
        return BuildEngineSnapshot(raw, output, error);
      }
      return CollectSnapshotLocked(session, output, error);
    } catch (...) {
      *output = EngineSnapshot{};
      SetError(error, "exception while processing a librime key event");
      return false;
    }
  }

  bool CommitComposition(SessionId session, EngineSnapshot* output, std::string* error) noexcept {
    if (!output) return false;
    *output = {};
    try {
      std::lock_guard lock(mutex_);
      if (!healthy_ || !api_ || !api_->commit_composition || !sessions_.contains(session)) {
        SetError(error, "unknown or inactive librime session"); return false;
      }
      api_->commit_composition(session);
      return CollectSnapshotLocked(session, output, error);
    } catch (...) {
      SetError(error, "exception while committing librime composition"); return false;
    }
  }

  bool ProcessShiftTap(SessionId session, std::int32_t keycode,
                       std::int32_t modifiers, EngineSnapshot* output,
                       std::string* error) noexcept {
    if (output) *output = {};
    constexpr int release = 1 << 30;
    constexpr int commands = (1 << 2) | (1 << 3) | (1 << 6);
    if (!output || (keycode != 0xffe1 && keycode != 0xffe2) ||
        (modifiers & commands) != 0) return false;
    try {
      std::lock_guard lock(mutex_);
      if (!healthy_ || !api_ || !sessions_.contains(session)) return false;
      // Preserve the schema's own switch style. This is a qualified gesture,
      // never a retry of a failed physical key or an injected host shortcut.
      api_->process_key(session, keycode, (modifiers & ~release) | 1);
      api_->process_key(session, keycode, modifiers | release);
      if (!CollectSnapshotLocked(session, output, error)) return false;
      output->handled = false;
      output->modifier_snapshot = true;
      return true;
    } catch (...) {
      SetError(error, "exception while processing a Shift tap");
      return false;
    }
  }

  bool DeployDefaultConfiguration(std::string* error) noexcept {
    try {
      std::lock_guard lock(mutex_);
      if (!healthy_ || !api_ || !api_->deploy_config_file ||
          !api_->deploy_config_file("default.yaml", "config_version")) {
        SetError(error, "librime could not compile default.yaml"); return false;
      }
      return true;
    } catch (...) { SetError(error, "exception while compiling default.yaml"); return false; }
  }

  bool DeploySchema(const std::filesystem::path& path, std::string* error) noexcept {
    try {
      std::lock_guard lock(mutex_);
      const auto encoded = path.u8string();
      const std::string filename(encoded.begin(), encoded.end());
      if (!healthy_ || !api_ || !api_->deploy_schema || !api_->deploy_schema(filename.c_str())) {
        SetError(error, "librime could not compile the schema"); return false;
      }
      return true;
    } catch (...) { SetError(error, "exception while compiling a schema"); return false; }
  }

  bool Configure(SessionId session, const std::string& schema, bool ascii,
                 bool traditional, bool punctuation,
                 std::string* error, unsigned candidate_count,
                 const std::map<std::string, bool>& input_options) noexcept {
    try {
      std::lock_guard lock(mutex_);
      if (!healthy_ || !sessions_.contains(session) || !api_ ||
          api_->data_size <
              static_cast<int>(offsetof(abi::ApiPrefix, config_set_int) +
                               sizeof(api_->config_set_int) -
                               sizeof(api_->data_size)) ||
          !api_->select_schema || !api_->set_option || !api_->clear_composition ||
          !api_->schema_open || !api_->config_close || !api_->config_set_int ||
          candidate_count < 1 || candidate_count > 9)
        return false;
      abi::Config config{};
      if (!api_->schema_open(schema.c_str(), &config)) {
        SetError(error, "Schema configuration unavailable");
        return false;
      }
      const bool page_set = api_->config_set_int(
          &config, "menu/page_size", static_cast<int>(candidate_count));
      if (!page_set) {
        api_->config_close(&config);
        SetError(error, "Candidate count unavailable");
        return false;
      }
      api_->clear_composition(session);
      input_habits_[session].Restore(api_, session);
      input_habits_.erase(session);
      if (!api_->select_schema(session, schema.c_str())) {
        api_->config_close(&config);
        SetError(error, "Schema unavailable");
        return false;
      }
      // Keep the config alive until ApplySchema acquires its shared data.
      // The deployed schema component does not auto-save: custom YAML remains untouched.
      api_->config_close(&config);
      candidate_page_keys_.erase(session);
      api_->set_option(session, "ascii_mode", ascii ? 1 : 0);
      api_->set_option(session, "traditionalization", traditional ? 1 : 0);
      api_->set_option(session, "ascii_punct", punctuation ? 1 : 0);
      input_habits_[session].Update<abi::Config>(api_, session, schema, input_options);
      return true;
    } catch (...) {
      SetError(error, "Engine configuration failed");
      return false;
    }
  }

  std::vector<xsime::input::Switch> InputOptions(const std::string& schema) noexcept {
    try {
      std::lock_guard lock(mutex_);
      return healthy_ ? xsime::input::Switches<abi::Config>(api_, schema) : std::vector<xsime::input::Switch>{};
    } catch (...) { return {}; }
  }

  bool UserDictionaries(std::vector<std::string>* names, std::string* error) noexcept {
    try {
      std::lock_guard lock(mutex_);
      if (!names) return false;
      names->clear();
      auto* levers = LeversLocked(error);
      if (!levers) return false;
      abi::UserDictIterator iterator{};
      if (!levers->user_dict_iterator_init(&iterator)) {
        SetError(error, "Rime 无法枚举学习词库。"); return false;
      }
      struct End {
        abi::LeversApi* api; abi::UserDictIterator* iterator;
        ~End() { api->user_dict_iterator_destroy(iterator); }
      } end{levers, &iterator};
      for (unsigned i = 0; i < 4096; ++i) {
        const auto* name = levers->next_user_dict(&iterator);
        if (!name) return true;
        std::string copy;
        if (!CopyBoundedCString(name, 1024, &copy, error, "user dictionary")) return false;
        names->push_back(std::move(copy));
      }
      SetError(error, "Rime 学习词库枚举超过安全上限。"); return false;
    } catch (...) { SetError(error, "Rime 学习词库枚举失败。"); return false; }
  }
  int TransferUserDictionary(const std::string& name, const std::filesystem::path& path,
                             bool importing, std::string* error) noexcept {
    try {
      std::lock_guard lock(mutex_);
      if (error) error->clear();
      auto* levers = LeversLocked(error);
      if (!levers) return -1;
      if (name != "rime_ice" && name != "wubi86" && name != "english") {
        SetError(error, "未知学习词库。"); return -1;
      }
      std::string filename;
      if (!LexiconPath(path, &filename, error)) return -1;
      const auto result = WithUserDictionariesClosed([&] {
        return importing ? levers->import_user_dict(name.c_str(), filename.c_str())
                         : levers->export_user_dict(name.c_str(), filename.c_str());
      }, error);
      if (result < 0 && (!error || error->empty()))
        SetError(error, importing ? "Rime 未能导入学习词库。" : "Rime 未能导出学习词库。");
      return result;
    } catch (...) { SetError(error, "Rime 学习词库传输失败。"); return -1; }
  }
  bool RestoreUserDictionary(const std::filesystem::path& path, std::string* error) noexcept {
    try {
      std::lock_guard lock(mutex_);
      if (error) error->clear();
      auto* levers = LeversLocked(error);
      if (!levers) return false;
      std::string filename;
      if (!LexiconPath(path, &filename, error)) return false;
      const bool ok = WithUserDictionariesClosed([&] {
        return levers->restore_user_dict(filename.c_str()) ? 1 : -1;
      }, error) >= 0;
      if (!ok && (!error || error->empty())) SetError(error, "Rime 未能恢复学习词库快照。");
      return ok;
    } catch (...) { SetError(error, "Rime 学习词库快照恢复失败。"); return false; }
  }

 private:
  int WithUserDictionariesClosed(const std::function<int()>& operation, std::string* error) {
    if (sessions_.empty()) return operation();
    constexpr auto required = offsetof(abi::ApiPrefix, config_set_item) + sizeof(abi::ApiPrefix::config_set_item) - sizeof(int);
    if (api_->data_size < static_cast<int>(required) ||
        !api_->get_current_schema || !api_->select_schema || !api_->config_get_item || !api_->config_set_item ||
        !api_->config_load_string || !api_->schema_open || !api_->config_close ||
        !api_->get_option || !api_->set_option) {
      SetError(error, "Rime 暂不支持安全切换学习词库。"); return -1;
    }
    struct SessionState {
      SessionId id; std::string schema; std::map<std::string, bool> options;
      abi::Config config{}, components{};
      bool config_changed = false, switched = false;
    };
    struct Lease {
      abi::ApiPrefix* api;
      bool& healthy;
      std::vector<SessionState> sessions;
      abi::Config guard{};
      bool Restore() noexcept {
        bool ok = true;
        // Restore every shared schema tree before recreating any translator.
        // Multiple live sessions may share the same underlying ConfigData.
        for (auto& session : sessions) if (session.config_changed) {
          try {
            if (api->config_set_item(&session.config, "engine", &session.components)) session.config_changed = false;
            else ok = false;
          } catch (...) { ok = false; }
        }
        if (!ok) return false;
        for (auto& session : sessions) if (session.switched) {
          try {
            if (!api->select_schema(session.id, session.schema.c_str())) { ok = false; continue; }
            for (const auto& [name, value] : session.options) api->set_option(session.id, name.c_str(), value ? 1 : 0);
            session.switched = false;
          } catch (...) { ok = false; }
        }
        return ok;
      }
      ~Lease() {
        // Keep failed sessions pending for another recovery attempt. If the
        // native API is irrecoverable, reject further keys rather than run a
        // session with no translators and silently consume user input.
        if (!Restore() && !Restore()) healthy = false;
        for (auto& session : sessions) {
          if (session.components.ptr) api->config_close(&session.components);
          if (session.config.ptr) api->config_close(&session.config);
        }
        if (guard.ptr) api->config_close(&guard);
      }
    } lease{api_, healthy_};
    lease.sessions.reserve(sessions_.size());
    // Check every session before changing any schema. Never commit, cancel or
    // replay composition as a side effect of a settings-file operation.
    for (const auto id : sessions_) {
      abi::Context context; abi::InitializeVersionedStruct(&context);
      if (!api_->get_context(id, &context)) { SetError(error, "无法核验输入会话状态。"); return -1; }
      const bool composing = context.composition.length > 0 || context.menu.num_candidates > 0;
      api_->free_context(&context);
      if (composing) { SetError(error, "请先完成当前组字，再导入或导出学习词库。"); return -1; }
      char schema[1025]{};
      if (!api_->get_current_schema(id, schema, sizeof(schema)) || !std::memchr(schema, 0, sizeof(schema))) {
        SetError(error, "无法保存当前输入方案。"); return -1;
      }
      lease.sessions.push_back({id, schema});
      auto& saved = lease.sessions.back();
      if (!api_->schema_open(saved.schema.c_str(), &saved.config) ||
          !api_->config_get_item(&saved.config, "engine", &saved.components)) {
        SetError(error, "无法保留当前方案配置。"); return -1;
      }
      std::set<std::string> names{"ascii_mode", "ascii_punct", "full_shape", "simplification", "traditional",
          "traditionalization", "_horizontal", "_soft_cursor", "_inline_preedit", "_hide_comment", "_hide_candidate"};
      // Preserve every declared binary/multi-option/hidden switch, not only
      // the subset exposed by the Windows input-habits settings.
      if (api_->config_get_string) for (unsigned i = 0; i < 128; ++i) {
        const auto base = "switches/@" + std::to_string(i);
        char name[256]{};
        if (api_->config_get_string(&saved.config, (base + "/name").c_str(), name, sizeof(name)) &&
            std::memchr(name, 0, sizeof(name))) names.insert(name);
        for (unsigned j = 0; j < 128; ++j) {
          memset(name, 0, sizeof(name));
          if (!api_->config_get_string(&saved.config, (base + "/options/@" + std::to_string(j)).c_str(), name, sizeof(name))) break;
          if (std::memchr(name, 0, sizeof(name))) names.insert(name);
        }
      }
      for (const auto& name : names) if (!name.empty()) saved.options[name] = api_->get_option(id, name.c_str()) != 0;
    }
    // Keep the original schema ID: select_schema persists schema IDs/history.
    // Temporarily replace only its shared in-memory component subtree, then
    // put back the exact original ConfigItem before restoring the sessions.
    if (!api_->config_load_string(&lease.guard,
        "processors: []\nsegmentors: []\ntranslators: []\nfilters: []\n")) {
      SetError(error, "无法准备安全的学习词库操作。"); return -1;
    }
    for (auto& session : lease.sessions) {
      session.config_changed = true;
      if (!api_->config_set_item(&session.config, "engine", &lease.guard)) {
        SetError(error, "无法暂时释放学习词库配置；输入方案将恢复。"); return -1;
      }
    }
    for (auto& session : lease.sessions) {
      // Include the attempted session in recovery even if an API implementation
      // reports failure after applying part of the switch.
      session.switched = true;
      if (!api_->select_schema(session.id, session.schema.c_str())) {
        SetError(error, "无法暂时释放学习词库；输入方案将恢复。"); return -1;
      }
    }
    const int result = operation();
    if (!lease.Restore() && !lease.Restore()) {
      healthy_ = false;
      SetError(error, "学习词库操作后输入引擎恢复失败，已停止处理按键，请重新启动输入法。"); return -1;
    }
    return result;
  }
  static bool LexiconPath(const std::filesystem::path& path, std::string* text, std::string* error) {
    if (!path.is_absolute() || path.native().size() > kMaximumWindowsPath ||
        path.native().find(L'\0') != std::wstring::npos) {
      SetError(error, "学习词库路径无效。"); return false;
    }
    return WideToUtf8(path.native(), text, error);
  }
  abi::LeversApi* LeversLocked(std::string* error) {
    if (!healthy_ || !api_ || !api_->find_module) {
      SetError(error, "Rime 引擎尚未就绪。"); return nullptr;
    }
    const auto* module = api_->find_module("levers");
    constexpr auto module_size = offsetof(abi::Module, get_api) + sizeof(abi::Module::get_api) - sizeof(int);
    if (!module || module->data_size < static_cast<int>(module_size) ||
        module->data_size > static_cast<int>(kMaximumApiBytes) || !module->get_api) {
      SetError(error, "此 Rime 引擎未提供学习词库接口。"); return nullptr;
    }
    auto* levers = static_cast<abi::LeversApi*>(module->get_api());
    constexpr auto required = offsetof(abi::LeversApi, import_user_dict) + sizeof(abi::LeversApi::import_user_dict) - sizeof(int);
    if (!levers || levers->data_size < static_cast<int>(required) ||
        levers->data_size > static_cast<int>(kMaximumApiBytes) ||
        !levers->user_dict_iterator_init || !levers->user_dict_iterator_destroy || !levers->next_user_dict ||
        !levers->export_user_dict || !levers->import_user_dict || !levers->restore_user_dict) {
      SetError(error, "此 Rime 引擎的学习词库接口不完整。"); return nullptr;
    }
    return levers;
  }
  struct OwnedCandidate {
    std::string text;
    std::string comment;
    std::string label;
  };

  bool StoreDataPaths(const RimeEngineOptions& options,
                      std::string* error) noexcept {
    try {
      if (options.shared_data_dir.empty() ||
          !options.shared_data_dir.is_absolute() ||
          options.shared_data_dir.native().size() > kMaximumWindowsPath) {
        SetError(error, "shared data directory must be absolute");
        return false;
      }
      if (options.user_data_dir.empty() ||
          !options.user_data_dir.is_absolute() ||
          options.user_data_dir.native().size() > kMaximumWindowsPath) {
        SetError(error, "user data directory must be absolute");
        return false;
      }
      if (!options.log_dir.empty() &&
          (!options.log_dir.is_absolute() ||
           options.log_dir.native().size() > kMaximumWindowsPath)) {
        SetError(error, "log directory must be absolute");
        return false;
      }
      return WideToUtf8(options.shared_data_dir.native(), &shared_data_dir_,
                        error) &&
             WideToUtf8(options.user_data_dir.native(), &user_data_dir_,
                        error) &&
             WideToUtf8(options.log_dir.native(), &log_dir_, error);
    } catch (...) {
      SetError(error, "exception while validating librime data paths");
      return false;
    }
  }

  bool RunMaintenanceLocked(bool full_check, std::string* error) noexcept {
    if (api_ == nullptr || api_->start_maintenance == nullptr ||
        api_->is_maintenance_mode == nullptr ||
        api_->join_maintenance_thread == nullptr) {
      SetError(error, "librime maintenance API is unavailable");
      return false;
    }
    const bool started = api_->start_maintenance(full_check ? 1 : 0) != 0;
    if (started || api_->is_maintenance_mode() != 0) {
      api_->join_maintenance_thread();
    }
    return true;
  }

  std::uint32_t CandidatePageKeysLocked(SessionId session) {
    constexpr auto required = offsetof(abi::ApiPrefix, config_get_string) +
        sizeof(abi::ApiPrefix::config_get_string) - sizeof(int);
    if (!api_ || api_->data_size < static_cast<int>(required) ||
        !api_->get_current_schema || !api_->schema_open || !api_->config_close ||
        !api_->config_get_string) return 0;
    char schema[256]{};
    if (!api_->get_current_schema(session, schema, sizeof(schema)) || !schema[0] ||
        !memchr(schema, '\0', sizeof(schema))) return 0;
    const auto found = candidate_page_keys_.find(session);
    if (found != candidate_page_keys_.end() && found->second.first == schema) return found->second.second;
    abi::Config config{};
    if (!api_->schema_open(schema, &config)) return 0;
    struct CloseConfig {
      abi::ApiPrefix* api; abi::Config* config;
      ~CloseConfig() { api->config_close(config); }
    } close{api_, &config};
    const auto flags = ReadCandidatePageKeys([&](const std::string& key) {
      char value[256]{};
      return api_->config_get_string(&config, key.c_str(), value, sizeof(value)) &&
          memchr(value, '\0', sizeof(value)) ? std::string(value) : std::string();
    });
    candidate_page_keys_[session] = {schema, flags};
    return flags;
  }

  bool CollectSnapshotLocked(SessionId session, EngineSnapshot* output,
                             std::string* error) noexcept {
    std::string commit_text;
    abi::Commit commit;
    abi::InitializeVersionedStruct(&commit);
    if (api_->get_commit(session, &commit) != 0) {
      const bool copied = CopyBoundedCString(
          commit.text, core::kMaxCommitTextBytes, &commit_text, error, "commit");
      api_->free_commit(&commit);
      if (!copied) {
        return false;
      }
    }

    abi::Context context;
    abi::InitializeVersionedStruct(&context);
    const bool has_context = api_->get_context(session, &context) != 0;

    std::string composition;
    std::string select_keys;
    std::vector<OwnedCandidate> candidates;
    std::size_t caret_utf8 = 0;
    std::size_t selection_start_utf8 = 0;
    std::size_t selection_end_utf8 = 0;
    std::uint32_t page_number = 0;
    std::uint32_t page_size = 0;
    bool last_page = false;
    int highlighted_candidate = -1;
    bool copied = true;

    if (has_context) {
      struct ContextGuard {
        abi::ApiPrefix* api;
        abi::Context* context;
        ~ContextGuard() { api->free_context(context); }
      } context_guard{api_, &context};

      if (context.composition.length < 0 ||
          context.composition.cursor_pos < 0 ||
          context.composition.sel_start < 0 ||
          context.composition.sel_end < 0 || context.menu.page_size < 0 ||
          context.menu.page_no < 0 || context.menu.num_candidates < 0 ||
          context.menu.num_candidates >
              static_cast<int>(core::kMaxCandidateCount)) {
        SetError(error, "librime returned negative or oversized context data");
        copied = false;
      }

      if (copied) {
        copied =
            CopyBoundedCString(context.composition.preedit,
                               core::kMaxCompositionBytes, &composition, error, "preedit");
      }
      if (copied && static_cast<std::size_t>(context.composition.length) >
                        composition.size()) {
        SetError(error, "librime composition length exceeds its preedit text");
        copied = false;
      }
      if (copied) {
        caret_utf8 = static_cast<std::size_t>(context.composition.cursor_pos);
        selection_start_utf8 =
            static_cast<std::size_t>(context.composition.sel_start);
        selection_end_utf8 =
            static_cast<std::size_t>(context.composition.sel_end);
      }

      const int count = copied ? context.menu.num_candidates : 0;
      if (copied && count > 0) {
        if (context.menu.candidates == nullptr || context.menu.page_size <= 0 ||
            context.menu.page_size >
                static_cast<int>(core::kMaxCandidateCount) ||
            count > context.menu.page_size) {
          SetError(error, "librime returned inconsistent candidate metadata");
          copied = false;
        } else {
          page_number = static_cast<std::uint32_t>(context.menu.page_no);
          page_size = static_cast<std::uint32_t>(context.menu.page_size);
          last_page = context.menu.is_last_page != 0;
          highlighted_candidate = context.menu.highlighted_candidate_index;
          candidates.resize(static_cast<std::size_t>(count));
        }
      }

      if (copied && count > 0 && context.menu.select_keys != nullptr) {
        copied =
            CopyBoundedCString(context.menu.select_keys,
                               kMaximumSelectKeysBytes, &select_keys, error, "candidate keys");
      }
      const std::vector<std::string_view> scalar_labels =
          copied ? SplitUtf8Scalars(select_keys)
                 : std::vector<std::string_view>{};

      for (int index = 0; copied && index < count; ++index) {
        OwnedCandidate& candidate = candidates[static_cast<std::size_t>(index)];
        copied = CopyBoundedCString(context.menu.candidates[index].text,
                                    core::kMaxCandidateTextBytes,
                                    &candidate.text, error, "candidate text") &&
                 CopyBoundedCString(context.menu.candidates[index].comment,
                                    core::kMaxCandidateCommentBytes,
                                    &candidate.comment, error, "candidate comment", true);
        if (!copied) {
          break;
        }

        if (context.select_labels != nullptr &&
            context.select_labels[index] != nullptr) {
          copied = CopyBoundedCString(context.select_labels[index],
                                      core::kMaxCandidateLabelBytes,
                                      &candidate.label, error, "candidate label");
        } else if (static_cast<std::size_t>(index) < scalar_labels.size()) {
          candidate.label.assign(
              scalar_labels[static_cast<std::size_t>(index)]);
        } else {
          candidate.label = std::to_string(index + 1);
        }
      }
      if (!copied) {
        return false;
      }
    }

    std::vector<RawCandidateView> candidate_views;
    candidate_views.reserve(candidates.size());
    for (const OwnedCandidate& candidate : candidates) {
      candidate_views.push_back(
          {candidate.text, candidate.comment, candidate.label});
    }

    RawSnapshotView raw;
    raw.handled = true;
    raw.has_context = has_context;
    raw.composition = composition;
    raw.commit_text = commit_text;
    raw.caret_utf8 = caret_utf8;
    raw.selection_start_utf8 = selection_start_utf8;
    raw.selection_end_utf8 = selection_end_utf8;
    raw.page_number = page_number;
    raw.page_size = page_size;
    raw.last_page = last_page;
    if (!candidate_views.empty()) raw.candidate_page_keys = CandidatePageKeysLocked(session);
    raw.highlighted_candidate = highlighted_candidate;
    raw.candidates = candidate_views;
    return BuildEngineSnapshot(raw, output, error);
  }

  void StopLocked() noexcept {
    healthy_ = false;
    if (api_ != nullptr) {
      for (const SessionId session : sessions_) {
        api_->destroy_session(session);
      }
      sessions_.clear(); input_habits_.clear(); candidate_page_keys_.clear();
      if (initialized_) {
        api_->finalize();
      }
    }
    initialized_ = false;
    api_ = nullptr;
    if (module_ != nullptr) {
      FreeLibrary(module_);
      module_ = nullptr;
    }
    shared_data_dir_.clear();
    user_data_dir_.clear();
    log_dir_.clear();
    if (owns_runtime_) {
      void* expected = this;
      g_runtime_owner.compare_exchange_strong(expected, nullptr);
      owns_runtime_ = false;
    }
  }

  mutable std::mutex mutex_;
  HMODULE module_ = nullptr;
  abi::ApiPrefix* api_ = nullptr;
  bool initialized_ = false;
  bool healthy_ = false;
  bool owns_runtime_ = false;
  std::string shared_data_dir_;
  std::string user_data_dir_;
  std::string log_dir_;
  std::unordered_set<SessionId> sessions_;
  std::map<SessionId, xsime::input::SessionOverrides> input_habits_;
  std::map<SessionId, std::pair<std::string, std::uint32_t>> candidate_page_keys_;
};

RimeEngine::RimeEngine() : impl_(std::make_unique<Impl>()) {}

RimeEngine::~RimeEngine() { Stop(); }

bool RimeEngine::DeploySchema(const std::filesystem::path& path, std::string* error) noexcept {
  return impl_->DeploySchema(path, error);
}

bool RimeEngine::DeployDefaultConfiguration(std::string* error) noexcept {
  return impl_->DeployDefaultConfiguration(error);
}

bool RimeEngine::Start(const RimeEngineOptions& options,
                       std::string* error) noexcept {
  return impl_->Start(options, error);
}

void RimeEngine::Stop() noexcept { impl_->Stop(); }

bool RimeEngine::IsHealthy() const noexcept { return impl_->IsHealthy(); }
bool RimeEngine::UserDictionaries(std::vector<std::string>* names, std::string* error) noexcept {
  return impl_->UserDictionaries(names, error);
}
int RimeEngine::ExportUserDictionary(const std::string& name, const std::filesystem::path& path,
                                     std::string* error) noexcept {
  return impl_->TransferUserDictionary(name, path, false, error);
}
int RimeEngine::ImportUserDictionary(const std::string& name, const std::filesystem::path& path,
                                     std::string* error) noexcept {
  return impl_->TransferUserDictionary(name, path, true, error);
}
bool RimeEngine::RestoreUserDictionary(const std::filesystem::path& path, std::string* error) noexcept {
  return impl_->RestoreUserDictionary(path, error);
}

bool RimeEngine::RunMaintenance(bool full_check, std::string* error) noexcept {
  return impl_->RunMaintenance(full_check, error);
}

RimeEngine::SessionId RimeEngine::CreateSession(std::string* error) noexcept {
  return impl_->CreateSession(error);
}

bool RimeEngine::DestroySession(SessionId session,
                                std::string* error) noexcept {
  return impl_->DestroySession(session, error);
}

bool RimeEngine::ProcessChordText(SessionId session, const std::string& text, const std::string& replacement,
                                  const EngineSnapshot& expected, EngineSnapshot* output,
                                  bool* replaced, std::string* error) noexcept {
  return impl_->ProcessChordText(session, text, replacement, expected, output, replaced, error);
}

bool RimeEngine::ProcessKey(SessionId session, std::int32_t keycode,
                            std::int32_t modifiers, EngineSnapshot* output,
                            std::string* error) noexcept {
  return impl_->ProcessKey(session, keycode, modifiers, output, error);
}

bool RimeEngine::CommitComposition(SessionId session, EngineSnapshot* output, std::string* error) noexcept {
  return impl_->CommitComposition(session, output, error);
}

bool RimeEngine::ProcessShiftTap(SessionId session, std::int32_t keycode,
                                 std::int32_t modifiers, EngineSnapshot* output,
                                 std::string* error) noexcept {
  return impl_->ProcessShiftTap(session, keycode, modifiers, output, error);
}

bool RimeEngine::Configure(SessionId session, const std::string& schema,
                           bool ascii, bool traditional, bool punctuation,
                           std::string* error, unsigned candidate_count,
                           const std::map<std::string, bool>& input_options) noexcept {
  return impl_->Configure(session, schema, ascii, traditional, punctuation,
                          error, candidate_count, input_options);
}
std::vector<xsime::input::Switch> RimeEngine::InputOptions(const std::string& schema) noexcept {
  return impl_->InputOptions(schema);
}

bool RimeEngine::IsAsciiMode(SessionId session) const noexcept {
  return impl_->IsAsciiMode(session);
}

}  // namespace rimes::windows::engine
