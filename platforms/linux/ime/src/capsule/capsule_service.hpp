#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <thread>
#include <vector>

#include <fcitx-utils/event.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/instance.h>

#include "background_jobs.hpp"
#include "capsule_model.hpp"
#include "capsule_protocol.hpp"
#include "capsule_store.hpp"
#include "capsule_transport.hpp"

namespace fcitx {

class CapsuleService {
public:
    using PostFn = std::function<void(std::function<void()>)>;

    CapsuleService(Instance* instance, PostFn post);
    ~CapsuleService();

    CapsuleService(const CapsuleService&) = delete;
    CapsuleService& operator=(const CapsuleService&) = delete;

    rimes::capsule::CapsuleModel& model() { return model_; }
    const rimes::capsule::CapsuleModel& model() const { return model_; }

    bool HandleEarlyKey(KeyEvent& event);
    void OnActivate(InputContext* ic);
    void OnDeactivate(InputContext* ic, bool switching_im);
    void OnInputContextDestroyed(InputContext* ic);
    void OnPasswordField(InputContext* ic, bool password);
    void OnSessionProtection(bool protected_session);
    void SetManagedHotkey() { managed_hotkey_ = true; }
    void ConfigureClipboard(bool capture);
    void ConfigureTheme(std::string theme) { if (theme_ != theme) { theme_ = std::move(theme); Publish(); } }
    void ShowDetached();

    void Toggle(InputContext* ic);
    void Show(InputContext* ic);
    void Close();
    void SetEnabled(bool enabled);
    bool Enabled() const { return enabled_ && !model_.enabled_modules().empty(); }
    void SetModuleEnabled(rimes::capsule::Module module, bool enabled);
    void BindModuleValidation(std::function<bool(rimes::capsule::Module)> validate) { validate_module_ = std::move(validate); }
    void Manage();
    void ReturnToRail();
    bool Activate();
    bool CopySelected();
    bool CopyResource(bool close);

    std::string SocketPath() const { return socket_path_; }
    void Publish();

private:
    bool AllowsModule();
    bool WantsClipboard() const;
    bool WantsUi() const { return model_.visible() || WantsClipboard(); }
    void ReloadHistory(int selected = -1, std::string selected_id = {});
    void MoveSelection(int delta);
    void SelectIndex(int index);
    bool HistoryAction(const std::string& action);
    bool CaptureAction(const std::string& action);
    bool clipboard_capture_ = false;
    int history_offset_ = 0, history_count_ = 0;
    std::string history_error_, history_scope_, capture_error_;
    std::shared_ptr<std::atomic<bool>> history_action_cancel_;
    std::string restore_id_, restore_revision_;
    std::uint64_t restore_seq_ = 0;
    bool restore_close_ = false;
    bool OpenDetail();
    void ClearDetail();
    bool RowAction(int index, std::string revision, bool insert);
    std::optional<rimes::capsule::Record> detail_;
    std::vector<rimes::capsule::DetailRow> detail_rows_;
    std::vector<std::string> detail_headings_;
    int detail_selected_ = -1;
    bool SelectIdentity(const rimes::capsule::Command& command);
    std::function<bool(rimes::capsule::Module)> validate_module_;
    rimes::runtime::BackgroundJobs storage_jobs_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> reload_cancel_;
    bool session_protected_ = false;
    bool field_protected_ = false;
    bool managed_hotkey_ = false;
    std::string theme_ = "night";
    struct Client {
        int fd = -1;
        std::string incoming;
        rimes::capsule::SnapshotWriter outgoing;
    };

    static std::string TokenFor(InputContext* ic);
    static bool IsToggleHotkey(const KeyEvent& event);
    static bool IsReturnKey(const Key& key);
    static bool IsPrintableAscii(const KeyEvent& event, char* out);

    InputContext* LiveTarget() const;
    bool ArmedFor(InputContext* ic) const;
    void ReloadNotes(std::string selected_id = {});
    void DropSession(std::string_view reason);
    void ArmFocusGraceTimer();
    void CancelFocusGraceTimer();
    bool DeliverNote(const rimes::capsule::Card& card);
    void HandleCommand(const rimes::capsule::Command& command);
    void EnsureUi();
    void ReapUi();
    void ReplaceDroppedUi();
    int UiClientCount();
    void StartSocket();
    void StopSocket();
    void SocketLoop();
    void AcceptClient();
    void WatchStorage();
    void StorageChanged();
    int watch_fd_ = -1, watch_directory_ = -1;
    std::unique_ptr<EventSourceTime> storage_timer_;
    void ReadClient(Client* client);
    void CloseClient(int fd);
    void WriteAll(const std::string& payload, bool revoke = false);
    std::string output_scope_;
    bool output_has_copy_ = false;
    void ScheduleUiRespawn();
    void ArmUiRespawnTimer(int delay_us);
    bool HasUiClients();

    Instance* instance_;
    PostFn post_;
    rimes::capsule::ContentStore store_;
    rimes::capsule::CapsuleModel model_;
    std::string socket_path_;
    std::string dump_path_;
    bool enabled_ = true;
    int listen_fd_ = -1, wake_fd_ = -1;
    std::vector<Client> clients_;
    std::recursive_mutex clients_mu_;
    std::thread socket_thread_;
    std::atomic<bool> running_{false};
    std::unique_ptr<EventSourceTime> focus_grace_timer_;
    std::unique_ptr<EventSourceTime> ui_respawn_timer_;
    pid_t ui_pid_ = 0;
    pid_t dropped_ui_pid_ = 0;
    int ui_respawn_attempt_ = 0;
    int focus_grace_ms_ = 5000;
    std::string pending_unfocus_token_;
    bool headless_ = false;
};

}  // namespace fcitx
