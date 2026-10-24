#pragma once
#include <bitset>
#include <condition_variable>
#include <deque>
#include <functional>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "../core/control.hpp"
#include "../engine/rime_snapshot.hpp"
#include "model.hpp"
#include "buffer_placement.hpp"
#include "provider.hpp"
#include "official_plugins.hpp"
#include "metrics.hpp"
#include "buffer_metrics.hpp"
#include "buffer_auto_send.hpp"
#include "../../../../../Shared/InputHabits/rime_options.hpp"
namespace rimes::windows::workbench {
class UserLexiconService;
class Runtime {
 public:
  std::function<std::vector<xsime::input::Switch>(const std::string&)> input_option_provider;
  std::function<bool(std::string*)> restart_broker;
  // Set before UI startup from the effective Broker engine configuration.
  std::filesystem::path log_directory, user_data_directory;
  std::shared_ptr<UserLexiconService> user_lexicon;
  using APIGenerator = std::function<bool(const Settings&, const Generation&,
      const std::function<bool(const std::string&)>&, const std::function<bool()>&, std::string*)>;
  struct TranslationResources {
    std::function<bool()> ready;
    std::function<bool(const std::function<bool()>&, std::string*)> prepare;
  };
  struct Readiness { bool ready = false; std::string message; };
  using ReadinessProbe = std::function<Readiness(const Settings&, const std::function<bool()>&)>;
  explicit Runtime(APIGenerator generate = GenerateText, TranslationResources resources = {},
                   std::function<std::uint64_t()> clock = [] { return GetTickCount64(); },
                   std::function<std::uint32_t()> foreground = [] {
                     DWORD process = 0; GetWindowThreadProcessId(GetForegroundWindow(), &process); return process;
                   }, ReadinessProbe readiness = {});
  ~Runtime();
  Target Register(std::uint32_t process, std::uint64_t session,
                  std::uint64_t context);
  void Remove(Target target);
  void Focus(Target target);
  bool Capturing(Target target);
  unsigned InputRoute(Target target);
  bool IsSendChord(const core::KeyEvent& key);
  int PluginCycleDirection(const core::KeyEvent& key);
  bool PluginShortcut(std::uint32_t foreground_process, unsigned key, unsigned modifiers);
  bool BeforeKey(Target target, const core::KeyEvent& key, bool composing);
  void Capture(Target target, engine::EngineSnapshot* snapshot,
               bool direct_input = false);
  // Called only on the owning engine connection. Validate the pending UI action
  // before touching Rime; callback must not re-enter Runtime.
  bool CompleteBufferTransition(Target target, std::uint64_t request,
      const std::function<bool(engine::EngineSnapshot*)>& settle);
  core::Json Control(const core::Json& message, std::uint32_t process);
  core::Json Snapshot();
  Target PlacementTarget();
  std::optional<BufferRect> PlacementCaret(Target target);
  void CancelOpeningPlacement();
  Settings Configuration();
  std::uint64_t ConfigurationRevision();
  void AppearanceChanged();
  void SetImportedSchema(std::string schema, std::string package = "imported");
  bool Configure(Settings value, const std::wstring& key, bool replace_key,
                 std::string* error, const std::map<std::string, std::wstring>& secrets = {});
  void Toggle(std::uint32_t foreground_process);
  void Bind(std::uint32_t foreground_process);
  // A settings window takes keyboard focus without closing the workbench or
  // invalidating a request frozen against the unchanged source/configuration.
  void PauseCapture();
  void ObserveForeground(std::uint32_t process);
  // Capsule borrows only this exact TSF lease. UI consumes a bounded queue on
  // its own thread; broker key handling never calls window code.
  bool BorrowPeripheral(std::uint32_t foreground_process);
  void ReleasePeripheral();
  void PeripheralPhysical(bool enabled);
  core::Json TakePeripheralInput();
  void PublishPeripheralCaret(const RECT& caret);
  bool SendPeripheral(const std::string& text);
  struct PeripheralPasteTicket {
    Target target;
    std::uint64_t focus_revision = 0;
    explicit operator bool() const { return static_cast<bool>(target); }
  };
  PeripheralPasteTicket PeripheralPasteTarget();
  bool CanPastePeripheral(PeripheralPasteTicket);
  void Close();
  void Protect();
  void DiscardBuffer();
  void Paste(std::string text);
  // Explicit peripheral review only stages text with no target or capture
  // authority. A later user bind must acquire a fresh TSF focus lease.
  bool StageExternal(std::string text);
  void Send(bool all);
  bool ShortcutDown(std::uint32_t foreground_process);
  void ShortcutUp();
  void SetInsertion(std::size_t index);
  void ReturnToEdit();
  void SetAutoSend(unsigned seconds, bool close_after);
  unsigned TakeClipboardAction();
  core::Json PrepareCopy();
  bool ValidateCopy(const core::Json& ticket);
  bool CommitCopy(const core::Json& ticket, const std::function<bool()>& write);
  void PublishCaret(const RECT& caret);
  void Generate(bool translation);
  void SelectAIMode();
  void SelectTranslationMode();
  std::vector<PluginView> BufferPlugins();
  bool SelectBufferPlugin(const std::string& id);
  bool SetTranslationLanguages(std::string source, std::string target);
  bool SetChatGPTInference(std::string model, std::string effort);
  void SetTranslationSpeech(bool enabled);
  void RequestTranslationSpeech(std::uint64_t block);
  core::Json TakeTranslationSpeech();
  // Stop queued/in-flight automatic translation when changing the displayed
  // mode. Completed results remain in their plugin workspace; an issued
  // delivery blocks selection until acknowledged. Selection never sends text.
  void ReturnToInput();
  void Cancel();
  void Tick();
  void Stop();
  void SetNotify(std::function<void()> notify);
  bool Stopping();
  std::vector<PluginView> Plugins();
  bool ManagePlugin(const std::string& id, const std::string& action, std::string* error);
  std::string PluginStatus();
  std::shared_ptr<metrics::Service> Metrics() { return metrics_; }

 private:
  struct Entry {
    Target target;
    std::deque<core::Json> events;
  };
  std::mutex mutex_;
  std::condition_variable event_, api_event_;
  std::map<std::uint64_t, Entry> sessions_;
  Model model_;
  std::string imported_schema_;
  std::string imported_package_;
  Settings settings_;
  OfficialPluginStore plugins_;
  std::shared_ptr<metrics::Service> metrics_ = std::make_shared<metrics::Service>();
  std::uint64_t metrics_save_at_ = 0;
  std::string result_plugin_, result_grant_, plugin_status_;
  std::string result_connector_, result_connector_grant_;
  bool plugin_installing_ = false;
  std::jthread plugin_worker_;
  TranslationResources translation_resources_;
  std::jthread translation_worker_;
  bool translation_ready_ = false, translation_preparing_ = false;
  bool translation_pending_ = false;
  std::uint64_t translation_retry_at_ = 0;
  std::string translation_status_;
  void PrepareTranslationResources(); // mutex held; never changes Buffer content/status
  std::function<void()> notify_;
  bool stopping_ = false, settings_valid_ = true;
  std::jthread api_worker_;
  APIGenerator generate_;
  ReadinessProbe readiness_probe_;
  std::jthread readiness_worker_;
  bool readiness_running_ = false;
  struct CachedReadiness { Readiness value; std::uint64_t expires = 0; };
  std::map<std::string, CachedReadiness> readiness_cache_;
  bool EffectiveGenerationSettings(bool translation, Settings* effective);
  Readiness GenerationReadinessLocked(bool translation);
  void RefreshReadinessLocked(bool translation);
  std::uint64_t generation_started_ = 0, generation_elapsed_second_ = 0;
  std::uint32_t last_external_process_ = 0;
  void ObserveAppSwitchLocked(std::uint32_t process);
  std::optional<std::pair<Settings, Generation>> api_job_;
  std::uint64_t pressed_at_ = 0, pending_since_ = 0, edited_at_ = 0;
  Target return_target_;
  Target peripheral_target_;
  std::uint64_t peripheral_delivery_ = 0;
  bool peripheral_physical_ = false;
  std::string peripheral_preedit_;
  core::Json peripheral_input_ = core::Json::array();
  void RevokePeripheral(); // mutex held
  std::uint64_t opening_revision_ = 0;
  std::uint64_t placement_revision_ = 0, opening_at_ = 0, opening_source_revision_ = 0;
  std::optional<BufferRect> opening_caret_;
  bool focus_follow_ = false, folded_ = false;
  std::optional<std::uint64_t> fold_at_;
  struct FocusRequest { Target target; std::uint64_t request, deadline; };
  std::optional<FocusRequest> focus_request_;
  Target foreground_suspended_{};
  std::uint64_t next_focus_request_ = 0;
  std::function<std::uint32_t()> foreground_;
  void FocusLocked(Target target);
  std::uint64_t peripheral_focus_revision_ = 1;
  bool return_held_ = false, return_sent_ = false, return_progress_ = false;
  unsigned return_key_ = 0;
  bool ai_mode_ = false;
  std::string ai_plugin_;
  std::string remembered_buffer_plugin_;
  struct SavedPluginResults {
    Model::Results results;
    std::string grant, connector, connector_grant;
    std::uint64_t configuration_revision = 0;
  };
  std::map<std::string, SavedPluginResults> saved_plugin_results_;
  void SwitchPluginResultsLocked(const std::string& id);
  std::uint64_t delivery_opening_revision_ = 0;
  void RememberBufferPluginLocked(const std::string& id);
  Target cycle_target_;
  std::bitset<256> cycle_held_;
  std::string SelectedBufferPluginLocked() const;
  std::vector<PluginView> BufferPluginsLocked();
  bool SelectBufferPluginLocked(const std::string& id);
  bool CycleBufferPluginLocked(int direction);
  unsigned clipboard_action_ = 0;
  Target clipboard_target_;
  std::uint32_t preedit_caret_ = 0;
  BufferAutoSendClock auto_clock_;
  BufferLiveMetrics live_metrics_;
  bool auto_countdown_active_ = false;
  std::uint64_t failed_translation_revision_ = 0;
  std::uint64_t translation_waiting_since_ = 0;
  bool translation_paused_ = false;
  std::function<std::uint64_t()> clock_;
  core::Json speech_request_;
  bool speech_after_delivery_ = false;
  RECT buffer_caret_{};
  void Changed();
  void DiscardBufferLocked();
  void CheckPluginAuthorization();
  std::optional<Delivery> SendAuthorized(bool all);
  void Queue(const std::optional<Delivery>& delivery);
  void CaptureChanged(bool follow_focus = false);
  void CaptureLocked(Target target, engine::EngineSnapshot* snapshot, bool direct_input);
  struct BufferTransition {
    Target target;
    std::uint64_t request, opening, revision, deadline;
    std::string preedit, plugin;
    bool close;
  };
  std::optional<BufferTransition> buffer_transition_;
  std::uint64_t next_transition_ = 0;
  bool QueueBufferTransitionLocked(bool close, const std::string& plugin = {});
  void ClearBufferTransitionLocked();
  void BindLocked(std::uint32_t foreground_process);
  void StartGeneration(bool translation, bool complete_sentence_only = false);
  void RunAPI();
  core::Json CopyTicketLocked();
};
}  // namespace rimes::windows::workbench
