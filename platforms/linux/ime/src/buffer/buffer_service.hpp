#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcitx-utils/event.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/instance.h>

#include "buffer_model.hpp"
#include "buffer_auto_send.hpp"
#include "buffer_protocol.hpp"
#include "buffer_return.hpp"
#include "configuration.hpp"
#include "units.hpp"

namespace fcitx {

// Process-wide Default Buffer. The model lives in the Fcitx5 addon; the GTK
// workbench is a renderer that speaks length-prefixed JSON over a Unix socket.
class BufferService {
public:
    using PostFn = std::function<void(std::function<void()>)>;
    struct ResultUpdate {
        unsigned alternative = 0;
        std::string text;
        bool finished = false, success = true;
        std::string message;
        std::vector<std::string> logical_blocks;
    };
    using ResultFn = std::function<void(ResultUpdate)>;
    using RunDerived = std::function<void(rimes::buffer::DerivedRequest,
        rimes::runtime::RequestConfiguration, std::shared_ptr<std::atomic<bool>>, ResultFn)>;

    BufferService(Instance* instance, PostFn post);
    ~BufferService();

    BufferService(const BufferService&) = delete;
    BufferService& operator=(const BufferService&) = delete;

    rimes::buffer::BufferModel& model() { return model_; }
    const rimes::buffer::BufferModel& model() const { return model_; }

    bool OnCommit(InputContext* ic, std::string_view text);
    bool HandleEarlyKey(KeyEvent& event, bool composing);
    void AfterRime(InputContext* ic, KeyEvent& event, bool rime_handled,
                   bool composing, std::string_view preedit, std::string_view raw_input);
    void OnActivate(InputContext* ic);
    void OnDeactivate(InputContext* ic, bool switching_im);
    void OnInputContextDestroyed(InputContext* ic);
    void OnPasswordField(InputContext* ic, bool password);
    void OnSessionProtection(bool protected_session);
    void SetManagedHotkey() { managed_hotkey_ = true; }

    void Toggle(InputContext* ic);
    void ShowAndCapture(InputContext* ic);
    void CloseAndPause();
    bool SendNext();
    bool SendAll();

    std::string SocketPath() const { return socket_path_; }
    void Publish();
    void Configure(bool close_after_last, bool reset_on_app_switch, int width,
                   std::string revision, std::string theme);
    void ConfigureAutoSend(bool enabled, int seconds);
    void BindAutoSend(std::function<void()> cycle) { cycle_auto_send_ = std::move(cycle); }
    void BindCloseAfterLast(std::function<void()> toggle) { toggle_close_after_last_ = std::move(toggle); }
    void BindPlugins(rimes::plugins::Registry& registry,
        std::function<bool(const rimes::plugins::Lease&)> validate,
        std::function<rimes::runtime::RequestConfiguration(std::string_view)> freeze);
    // Only compiled adapters may register here, never renderer commands.
    void RegisterDerivedAdapter(std::string id, std::string capability, RunDerived run, bool live_expand = true);
    bool SelectPlugin(const std::string& id);
    bool Generate();
    void CancelDerived();
    const rimes::buffer::DerivedWorkspace& derived() const { return derived_; }
    const rimes::translation::Units& translation() const { return translation_; }
    bool TranslationTick(bool force = false);

private:
    bool restore_after_session_ = false;
    bool focus_suspended_ = false;
    bool managed_hotkey_ = false;
    struct Client {
        int fd = -1;
        std::string incoming;
    };

    static std::string TokenFor(InputContext* ic);
    static bool IsToggleHotkey(const KeyEvent& event);
    static bool IsReturnKey(const Key& key);
    static bool IsBackspaceKey(const Key& key);
    static bool IsPrintableAscii(const KeyEvent& event, char* out);

    InputContext* LiveTarget() const;
    void RefreshCaret(InputContext* ic);
    void EndDrag();
    void ConsumeDragBlip();
    void ArmDragHardTimer();
    void CancelDragHardTimer();
    void ClearClientPreedit(InputContext* ic);
    void ApplyReturnAction(rimes::buffer::ReturnGesture::Action action, InputContext* ic,
                           bool composing);
    bool Deliver(bool all);
    bool DeliverDerived(bool all);
    bool CanDeliverDerived();
    void SynchronizeResultDelivery();
    void HandleCommand(const rimes::buffer::Command& command);
    void EnsureUi();
    void ReapUi();
    void ReplaceDroppedUi();
    int UiClientCount();
    void StartSocket();
    void StopSocket();
    void SocketLoop();
    void AcceptClient();
    void ReadClient(Client* client);
    void CloseClient(int fd);
    void WriteAll(const std::string& payload);
    void StageOpenPreedit();
    void DropCaptureForSwitch(std::string_view reason);
    void ArmFocusGraceTimer();
    void CancelFocusGraceTimer();
    void ScheduleUiRespawn();
    void ArmUiRespawnTimer(int delay_us);
    void ArmHoldTimer();
    void CancelHoldTimer();
    void SynchronizeAutoSend();
    void AutoSendTick();
    bool HasUiClients();
    void SynchronizeDerived();
    bool Translating() const;
    bool TranslationActive() const;
    void SynchronizeTranslation();
    void CancelTranslation(bool clear);

    Instance* instance_;
    PostFn post_;
    rimes::buffer::BufferModel model_;
    rimes::buffer::ReturnGesture gesture_;
    rimes::buffer::AutoSendClock auto_send_clock_;
    std::unique_ptr<EventSourceTime> auto_send_timer_;
    std::function<void()> cycle_auto_send_;
    std::function<void()> toggle_close_after_last_;
    bool auto_send_ = false;
    bool auto_send_scheduled_ = false;
    int auto_send_seconds_ = 1;
    struct DerivedAdapter { std::string capability; RunDerived run; bool live_expand; };
    rimes::buffer::DerivedWorkspace derived_;
    std::map<std::string, DerivedAdapter> derived_adapters_;
    rimes::plugins::Registry* plugins_ = nullptr;
    std::function<bool(const rimes::plugins::Lease&)> validate_;
    std::function<rimes::runtime::RequestConfiguration(std::string_view)> freeze_;
    std::optional<rimes::buffer::DerivedRequest> derived_request_;
    std::shared_ptr<std::atomic<bool>> derived_cancel_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::string configuration_revision_, theme_ = "night";
    std::uint64_t focus_epoch_ = 0;
    bool synchronizing_derived_ = false;
    rimes::translation::Units translation_;
    std::string translation_scope_, translation_unit_, translation_ticket_;
    std::string translation_result_id_;
    bool translation_explicit_ = false;
    bool translation_source_invalid_ = false;
    std::vector<rimes::buffer::Block> result_pending_;
    std::string result_pending_id_;
    std::map<unsigned, std::vector<std::string>> derived_logical_blocks_;
    std::string derived_error_;
    bool delivery_conflict_ = false;
    std::optional<rimes::plugins::Lease> translation_lease_;
    std::optional<rimes::runtime::RequestConfiguration> translation_configuration_;
    std::shared_ptr<std::atomic<bool>> translation_cancel_;
    std::unique_ptr<EventSourceTime> translation_timer_;
    std::string socket_path_;
    std::string dump_path_;
    int listen_fd_ = -1;
    std::vector<Client> clients_;
    std::recursive_mutex clients_mu_;
    std::thread socket_thread_;
    std::atomic<bool> running_{false};
    std::unique_ptr<EventSourceTime> hold_timer_;
    std::unique_ptr<EventSourceTime> focus_grace_timer_;
    std::unique_ptr<EventSourceTime> ui_respawn_timer_;
    std::unique_ptr<EventSourceTime> drag_hard_timer_;
    pid_t ui_pid_ = 0;
    pid_t dropped_ui_pid_ = 0;
    int ui_respawn_attempt_ = 0;
    int focus_grace_ms_ = 5000;
    std::string pending_unfocus_token_;
    std::string target_token_;
    std::string raw_input_;
    std::string last_external_program_;
    bool reset_on_app_switch_ = false;
    int width_ = 760;
    bool dragging_ = false;
    bool auto_capture_ = false;
    bool headless_ = false;
    bool eat_return_until_release_ = false;
    std::uint64_t return_generation_ = 0;
};

}  // namespace fcitx
