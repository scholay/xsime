#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "rime_snapshot.hpp"
#include "../../../../../Shared/InputHabits/rime_options.hpp"

namespace rimes::windows::engine {

struct RimeEngineOptions {
  // Must be an absolute local path whose final component is rime.dll. The
  // release installer will point this at RIMES' bundled copy; accepting the
  // path as configuration keeps development smoke tests independent of any
  // particular Weasel version or install directory.
  std::filesystem::path dll_path;
  std::filesystem::path shared_data_dir;
  std::filesystem::path user_data_dir;
  std::filesystem::path log_dir;
  bool full_maintenance_check = false;
  // Deployment must not open a learned-word database merely for a smoke
  // session: LevelDB otherwise rotates its journal and manifest on upgrade.
  bool verify_input_session = true;
  bool skip_maintenance = false;
};

// Broker-only librime owner. This class must never be linked into RimesTsf.dll:
// librime, dictionaries, maintenance work, and untrusted data parsing stay out
// of application processes that load the TSF service.
class RimeEngine final {
 public:
  using SessionId = std::uintptr_t;

  RimeEngine();
  ~RimeEngine();

  RimeEngine(const RimeEngine&) = delete;
  RimeEngine& operator=(const RimeEngine&) = delete;
  RimeEngine(RimeEngine&&) = delete;
  RimeEngine& operator=(RimeEngine&&) = delete;

  bool Start(const RimeEngineOptions& options,
             std::string* error = nullptr) noexcept;
  void Stop() noexcept;
  [[nodiscard]] bool IsHealthy() const noexcept;

  // start_maintenance returning false means that no maintenance thread was
  // started, not necessarily an error. A started/in-progress thread is always
  // joined before this method returns.
  bool RunMaintenance(bool full_check, std::string* error = nullptr) noexcept;

  bool DeploySchema(const std::filesystem::path& schema, std::string* error = nullptr) noexcept;
  bool DeployDefaultConfiguration(std::string* error = nullptr) noexcept;

  SessionId CreateSession(std::string* error = nullptr) noexcept;
  bool DestroySession(SessionId session, std::string* error = nullptr) noexcept;
  [[nodiscard]] bool IsAsciiMode(SessionId session) const noexcept;

  bool Configure(SessionId session, const std::string& schema, bool ascii,
                 bool traditional, bool ascii_punctuation,
                 std::string* error = nullptr, unsigned candidate_count = 9,
                 const std::map<std::string, bool>& input_options = {}) noexcept;
  std::vector<xsime::input::Switch> InputOptions(const std::string& schema) noexcept;
  // Official levers calls share the same serial engine lock as keystrokes.
  // They merge learning records; they never finalize/restart the runtime or
  // close live input sessions to manipulate LevelDB files.
  bool UserDictionaries(std::vector<std::string>* names, std::string* error = nullptr) noexcept;
  int ExportUserDictionary(const std::string& name, const std::filesystem::path&,
                           std::string* error = nullptr) noexcept;
  int ImportUserDictionary(const std::string& name, const std::filesystem::path&,
                           std::string* error = nullptr) noexcept;
  bool RestoreUserDictionary(const std::filesystem::path&, std::string* error = nullptr) noexcept;

  // Applies one key and atomically drains its commit/context into output. An
  // ordinary unhandled key yields an empty snapshot; explicit Shift snapshots
  // report state changes independently of host key ownership.
  bool ProcessChordText(SessionId session, const std::string& text, const std::string& replacement,
                        const EngineSnapshot& expected, EngineSnapshot* output,
                        bool* replaced, std::string* error = nullptr) noexcept;
  bool ProcessKey(SessionId session, std::int32_t keycode,
                 std::int32_t modifiers, EngineSnapshot* output,
                 std::string* error = nullptr) noexcept;
  // Resolve the selected conversion, unlike Return which may commit raw code.
  bool CommitComposition(SessionId session, EngineSnapshot* output,
                         std::string* error = nullptr) noexcept;
  // One qualified physical tap; process both legal Rime phases under one
  // engine lock. Canceled host shortcuts never create Rime modifier state.
  bool ProcessShiftTap(SessionId session, std::int32_t keycode,
                       std::int32_t modifiers, EngineSnapshot* output,
                       std::string* error = nullptr) noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rimes::windows::engine
