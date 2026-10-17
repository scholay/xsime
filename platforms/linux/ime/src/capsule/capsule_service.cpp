#include "capsule_service.hpp"
#include "capsule_capture.hpp"
#include "capsule_password.hpp"
#include "session/protected_input.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/log.h>
#include <fcitx/inputcontextmanager.h>

#include "buffer_process.hpp"
#include "buffer_protocol.hpp"

namespace fcitx {
namespace {

FCITX_DEFINE_LOG_CATEGORY(rimes_capsule_log, "rimes.capsule");

constexpr int kDefaultFocusGraceMs = 5000;

int FocusGraceMsFromEnv() {
    const char* value = std::getenv("RIMES_CAPSULE_FOCUS_GRACE_MS");
    if (value == nullptr || value[0] == '\0') {
        return kDefaultFocusGraceMs;
    }
    char* end = nullptr;
    const auto parsed = std::strtol(value, &end, 10);
    if (end == value || parsed < 50 || parsed > 30000) {
        return kDefaultFocusGraceMs;
    }
    return static_cast<int>(parsed);
}

std::string DefaultSocketPath() {
    if (const char* override_path = std::getenv("RIMES_CAPSULE_SOCKET")) {
        if (override_path[0] != '\0') {
            return override_path;
        }
    }
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR")) {
        return std::string(runtime) + "/rimes-capsule.sock";
    }
    return "/tmp/rimes-capsule-" + std::to_string(geteuid()) + ".sock";
}

bool EnvFlag(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

int SetCloexec(int fd) {
    const int flags = fcntl(fd, F_GETFD, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
    const int status = fcntl(fd, F_GETFL, 0);
    if (status >= 0) {
        fcntl(fd, F_SETFL, status | O_NONBLOCK);
    }
    return fd;
}

std::string FindUiBinary() {
    if (const char* override_path = std::getenv("RIMES_CAPSULE_UI")) {
        if (override_path[0] != '\0') {
            return override_path;
        }
    }
    const char* candidates[] = {
        "/usr/libexec/rimes/rimes-capsule",
        "/usr/lib/rimes/rimes-capsule",
        "/usr/bin/rimes-capsule",
        "/usr/local/bin/rimes-capsule",
        nullptr,
    };
    for (const char* const* path = candidates; *path != nullptr; ++path) {
        if (access(*path, X_OK) == 0) {
            return *path;
        }
    }
    if (const char* path_env = std::getenv("PATH")) {
        std::string remaining = path_env;
        while (!remaining.empty()) {
            const auto split = remaining.find(':');
            const std::string dir = remaining.substr(0, split);
            const std::string candidate = dir + "/rimes-capsule";
            if (access(candidate.c_str(), X_OK) == 0) {
                return candidate;
            }
            if (split == std::string::npos) {
                break;
            }
            remaining = remaining.substr(split + 1);
        }
    }
    return {};
}

}  // namespace

CapsuleService::CapsuleService(Instance* instance, PostFn post)
    : instance_(instance),
      post_(std::move(post)),
      store_(rimes::capsule::ContentStore::DefaultRoot()),
      socket_path_(DefaultSocketPath()),
      dump_path_(std::getenv("RIMES_CAPSULE_DUMP") ? std::getenv("RIMES_CAPSULE_DUMP") : ""),
      focus_grace_ms_(FocusGraceMsFromEnv()),
      headless_(EnvFlag("RIMES_CAPSULE_HEADLESS")) {
    ReloadNotes();
    StartSocket();
    Publish();
}

CapsuleService::~CapsuleService() {
    alive_->store(false); storage_jobs_.Shutdown();
    CancelFocusGraceTimer();
    ui_respawn_timer_.reset();
    storage_timer_.reset();
    StopSocket();
    ReapUi();
}

std::string CapsuleService::TokenFor(InputContext* ic) {
    if (ic == nullptr) {
        return {};
    }
    return std::to_string(reinterpret_cast<std::uintptr_t>(ic));
}

bool CapsuleService::IsToggleHotkey(const KeyEvent& event) {
    if (event.isRelease()) {
        return false;
    }
    const auto& key = event.rawKey();
    const bool shift = key.states().test(KeyState::Shift);
    const bool ctrl = key.states().test(KeyState::Ctrl);
    const bool super = key.states().test(KeyState::Super);
    const bool letter_v = key.check(FcitxKey_V) || key.check(FcitxKey_v) ||
                          key.sym() == FcitxKey_V || key.sym() == FcitxKey_v;
    return letter_v && shift && (ctrl || super) && !(ctrl && super);
}

bool CapsuleService::IsReturnKey(const Key& key) {
    return key.check(FcitxKey_Return) || key.check(FcitxKey_KP_Enter);
}

bool CapsuleService::IsPrintableAscii(const KeyEvent& event, char* out) {
    if (event.isRelease() || out == nullptr) {
        return false;
    }
    const auto states = event.rawKey().states();
    if (states.test(KeyState::Ctrl) || states.test(KeyState::Alt) ||
        states.test(KeyState::Super)) {
        return false;
    }
    const auto sym = static_cast<std::uint32_t>(event.key().sym());
    if (sym < 0x20 || sym > 0x7e) {
        return false;
    }
    *out = static_cast<char>(sym);
    return true;
}

InputContext* CapsuleService::LiveTarget() const {
    InputContext* found = nullptr;
    if (!instance_) return nullptr;
    instance_->inputContextManager().foreachFocused([&](InputContext* ic) {
        if (model_.arms(TokenFor(ic))) {
            found = ic;
            return false;
        }
        return true;
    });
    return found;
}

bool CapsuleService::ArmedFor(InputContext* ic) const {
    return ic != nullptr && model_.arms(TokenFor(ic));
}

void CapsuleService::ReloadNotes(std::string selected_id) {
    if (model_.module() == rimes::capsule::Module::Temporary) {
        ReloadHistory(-1, std::move(selected_id));
        return;
    }
    if (reload_cancel_) reload_cancel_->store(true);
    reload_cancel_ = std::make_shared<std::atomic<bool>>(false);
    const auto cancelled = reload_cancel_;
    model_.set_loading(true);
    const auto root = store_.root();
    const bool load_captures = model_.module() == rimes::capsule::Module::Capture;
    const bool load_passwords = model_.module() == rimes::capsule::Module::Passwords;
    storage_jobs_.Run(cancelled, [this, root, cancelled, alive = alive_, post = post_, selected_id, load_captures, load_passwords] {
        rimes::capsule::ContentStore store(root);
        std::vector<rimes::capsule::Record> records;
        std::string error;
        std::vector<rimes::capsule::Card> captures, passwords;
        auto status = store.SeedIfNeeded(&error);
        if (status == rimes::capsule::StoreStatus::Ok) status = store.ListAll(&records, &error);
        try {
          if (load_passwords) for(const auto& value:rimes::capsule::PasswordStore(root).List()) passwords.push_back(rimes::capsule::PasswordCard(value));
          if (load_captures) {
            rimes::capsule::CaptureStore archive(root/"captures");
            archive.Prune(std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count());
            for(const auto& capture:archive.List()) captures.push_back(rimes::capsule::CaptureCard(capture));
            for(const auto& entry:records) if(rimes::capsule::RecordModule(entry)==rimes::capsule::Module::Capture &&
                std::none_of(captures.begin(),captures.end(),[&](const auto& card){return card.id==entry.id;})) {
                rimes::capsule::Card card;card.id=entry.id;card.revision=entry.revision;card.title=entry.title;
                card.module=rimes::capsule::Module::Capture;card.source=entry.summary;
                card.kind=entry.kind;card.filter=entry.kind==rimes::capsule::Kind::Video?"video":"image";
                card.preview=entry.summary;card.favorite=true;captures.push_back(std::move(card));
            }
          }
        } catch(const std::exception&) {}
        if (!alive->load() || cancelled->load()) return;
        post([this, alive, cancelled, status, selected_id, records = std::move(records), captures = std::move(captures), passwords = std::move(passwords)] {
            if (!alive->load() || cancelled->load()) return;
            model_.set_loading(false);
            if (status == rimes::capsule::StoreStatus::Ok) model_.LoadCards(records);
            else model_.LoadCards({});
            if(model_.module()==rimes::capsule::Module::Capture) model_.LoadModuleCards(rimes::capsule::Module::Capture,std::move(captures));
            if(model_.module()==rimes::capsule::Module::Passwords) model_.LoadModuleCards(rimes::capsule::Module::Passwords,std::move(passwords));
            if (!selected_id.empty()) { rimes::capsule::Command selection; selection.id = selected_id; SelectIdentity(selection); }
            Publish();
        });
    });
}
void CapsuleService::SetModuleEnabled(rimes::capsule::Module module, bool enabled) {
    model_.EnableModule(module, enabled);
    if (enabled) ReloadNotes();
    Publish();
}
bool CapsuleService::AllowsModule() {
    if (!Enabled() || !model_.ModuleEnabled(model_.module()) || model_.password_field()) return false;
    if (!validate_module_) return true;
    try { return validate_module_(model_.module()) && model_.ModuleEnabled(model_.module()); }
    catch (const std::exception&) { return false; }
}
void CapsuleService::Manage() {
    if (!Enabled() || model_.password_field()) return;
    model_.set_managing(true); model_.Disarm("manage");
    pending_unfocus_token_.clear(); CancelFocusGraceTimer();
    ReloadNotes(); model_.set_visible(true); EnsureUi(); Publish();
}
void CapsuleService::ReturnToRail() {
    model_.set_managing(false); model_.Disarm("rail");
    pending_unfocus_token_.clear(); CancelFocusGraceTimer();
    ReloadNotes(); model_.set_visible(true); EnsureUi(); Publish();
}
bool CapsuleService::SelectIdentity(const rimes::capsule::Command& command) {
    if (command.id.empty()) {
        if (command.index >= 0)
            SelectIndex(command.index);
        return true;
    }
    const auto& cards = model_.cards();
    for (std::size_t i = 0; i < cards.size(); ++i)
        if (cards[i].id == command.id &&
            (command.revision.empty() || cards[i].revision == command.revision)) {
            model_.Select(static_cast<int>(i));
            return true;
        }
    return false;
}

void CapsuleService::DropSession(std::string_view reason) {
    FCITX_UNUSED(reason);
    pending_unfocus_token_.clear();
    CancelFocusGraceTimer();
    model_.Disarm(reason);
    Publish();
}

void CapsuleService::OnPasswordField(InputContext* ic, bool password) {
    if (ic && !ic->hasFocus()) return;
    field_protected_ = password;
    if (password || session_protected_) {
        model_.Hide();
        model_.set_password_field(true);
        Publish();
        return;
    }
    if (model_.password_field()) {
        model_.set_password_field(false);
        Publish();
    }
}

void CapsuleService::OnSessionProtection(bool protected_session) {
    if (session_protected_ == protected_session) return;
    session_protected_ = protected_session;
    pending_unfocus_token_.clear(); CancelFocusGraceTimer();
    if (protected_session) model_.Hide();
    model_.set_password_field(session_protected_ || field_protected_);
    Publish();
}

void CapsuleService::Toggle(InputContext* ic) {
    if (!model_.visible()) {
        Show(ic);
        return;
    }
    // Visible but disarmed (field switch): one hotkey re-arms this IC.
    // A second press, while armed here, closes.
    if (ic != nullptr && !ArmedFor(ic)) {
        Show(ic);
        return;
    }
    Close();
}

void CapsuleService::Show(InputContext* ic) {
    model_.set_managing(false);
    if (!enabled_ || ic == nullptr) {
        return;
    }
    const bool password = ProtectedInput(ic->capabilityFlags());
    OnPasswordField(ic, password);
    if (password || model_.password_field()) {
        return;
    }
    ReloadNotes();
    model_.set_visible(true);
    model_.set_armed(true);
    model_.set_token(TokenFor(ic));
    pending_unfocus_token_.clear();
    CancelFocusGraceTimer();
    EnsureUi();
    Publish();
}

void CapsuleService::Close() {
    model_.Hide();
    pending_unfocus_token_.clear();
    CancelFocusGraceTimer();
    Publish();
}

void CapsuleService::ShowDetached() {
    model_.set_managing(false);
    if (!Enabled() || model_.password_field()) return;
    model_.Disarm("detached-open");
    pending_unfocus_token_.clear(); CancelFocusGraceTimer();
    ReloadNotes(); model_.set_visible(true); EnsureUi(); Publish();
}

void CapsuleService::SetEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (!enabled_) Close();
}

bool CapsuleService::DeliverNote(const rimes::capsule::Card& card) {
    if (!AllowsModule() || model_.managing()) return false;
    InputContext* ic = LiveTarget();
    if (ic == nullptr || card.payload.empty() || card.kind != rimes::capsule::Kind::Note) {
        return false;
    }
    if (ProtectedInput(ic->capabilityFlags()) || model_.password_field()) {
        return false;
    }
    if (!model_.arms(TokenFor(ic))) {
        return false;
    }
    ic->commitString(card.payload);
    return true;
}

bool CapsuleService::Activate() {
    if (!AllowsModule() || model_.managing()) { Publish(); return false; }
    const auto* card = model_.Selected();
    if (card == nullptr) {
        return false;
    }
    if (card->module == rimes::capsule::Module::Temporary)
        return HistoryAction("activate");
    if (card->module == rimes::capsule::Module::Resources) return CopyResource(true);
    if (card->module == rimes::capsule::Module::Capture || card->module == rimes::capsule::Module::Passwords) { Manage(); return true; }
    rimes::capsule::Record record;
    std::string error;
    if (store_.Get(card->id, &record, &error) != rimes::capsule::StoreStatus::Ok) {
        return false;
    }
    if (record.revision != card->revision) { ReloadNotes(); return false; }
    rimes::capsule::Card fresh = *card;
    fresh.payload = record.content;
    fresh.kind = record.kind;
    if (!DeliverNote(fresh)) {
        return false;
    }
    Close();
    return true;
}

bool CapsuleService::CopyResource(bool close) {
    const auto* card = model_.Selected();
    if (!AllowsModule() || !card || (card->module != rimes::capsule::Module::Resources && card->module != rimes::capsule::Module::Capture)) return false;
    Publish(); // Establish the action scope before creating the one-shot intent.
    restore_id_ = card->id; restore_revision_ = card->revision;
    restore_close_ = close; ++restore_seq_; Publish(); return true;
}
bool CapsuleService::CopySelected() {
    if (!AllowsModule()) { Publish(); return false; }
    const auto* card = model_.Selected();
    if (card && card->module == rimes::capsule::Module::Temporary)
        return HistoryAction("copy");
    if (card && (card->module == rimes::capsule::Module::Resources || card->module == rimes::capsule::Module::Capture)) return CopyResource(false);
    if (card == nullptr || card->kind != rimes::capsule::Kind::Note) {
        return false;
    }
    rimes::capsule::Record record;
    std::string error;
    if (store_.Get(card->id, &record, &error) != rimes::capsule::StoreStatus::Ok) {
        return false;
    }
    if (record.revision != card->revision || !AllowsModule()) { ReloadNotes(); return false; }
    model_.set_last_copied(record.content);
    Publish();
    return true;
}

bool CapsuleService::OpenDetail() {
    if (!AllowsModule() || model_.managing())
        return false;
    const auto* card = model_.Selected();
    if (card && card->module == rimes::capsule::Module::Temporary)
        return Activate();
    if (card && (card->module == rimes::capsule::Module::Resources || card->module == rimes::capsule::Module::Capture || card->module == rimes::capsule::Module::Passwords)) { Manage(); return true; }
    if (!card || card->kind != rimes::capsule::Kind::Note)
        return false;
    rimes::capsule::Record record;
    std::string error;
    if (store_.Get(card->id, &record, &error) != rimes::capsule::StoreStatus::Ok ||
        record.revision != card->revision) {
        ReloadNotes();
        return false;
    }
    ClearDetail();
    detail_ = std::move(record);
    if (!detail_->format_issue.empty()) {
        std::istringstream lines(detail_->content);
        std::string line;
        while (std::getline(lines, line))
            if (!line.empty()) {
                detail_rows_.push_back({"line", "", line, false});
                detail_headings_.push_back("原文");
            }
    } else
        for (const auto& section :
             rimes::capsule::NoteSections(detail_->content, detail_->header_fields)) {
            for (const auto& row : section.rows) {
                detail_rows_.push_back(row);
                detail_headings_.push_back(section.heading);
            }
        }
    if (!detail_rows_.empty())
        detail_selected_ = 0;
    Publish();
    return true;
}
void CapsuleService::ClearDetail() {
    detail_.reset();
    detail_rows_.clear();
    detail_headings_.clear();
    detail_selected_ = -1;
}
bool CapsuleService::RowAction(int index, std::string revision, bool insert) {
    if (!AllowsModule() || model_.managing() || !detail_ || detail_->revision != revision ||
        index < 0 || index >= static_cast<int>(detail_rows_.size()))
        return false;
    const auto* selected = model_.Selected();
    if (!selected || selected->id != detail_->id)
        return false;
    rimes::capsule::Record fresh;
    std::string error;
    if (store_.Get(detail_->id, &fresh, &error) != rimes::capsule::StoreStatus::Ok ||
        fresh.revision != revision) {
        ClearDetail();
        ReloadNotes();
        Publish();
        return false;
    }
    detail_selected_ = index;
    const auto text = detail_rows_[index].value;
    if (insert) {
        auto card = *selected;
        card.payload = text;
        return DeliverNote(card);
    }
    model_.set_last_copied(text);
    Publish();
    return true;
}

bool CapsuleService::HandleEarlyKey(KeyEvent& event) {
    if (!Enabled() || model_.managing()) return false;
    InputContext* ic = event.inputContext();
    if (ic == nullptr) {
        return false;
    }
    OnPasswordField(ic, ProtectedInput(ic->capabilityFlags()));
    if (!managed_hotkey_ && IsToggleHotkey(event)) {
        Toggle(ic);
        event.filterAndAccept();
        return true;
    }
    if (!ArmedFor(ic) || event.isRelease()) {
        return false;
    }
    const auto& key = event.rawKey();
    if (detail_) {
        bool handled = true;
        if (key.check(FcitxKey_Escape))
            ClearDetail();
        else if (IsReturnKey(key))
            RowAction(detail_selected_, detail_->revision, true);
        else if (key.check(FcitxKey_Up) || key.check(FcitxKey_Down)) {
            if (!detail_rows_.empty())
                detail_selected_ = std::clamp(detail_selected_ + (key.check(FcitxKey_Up) ? -1 : 1),
                                              0, static_cast<int>(detail_rows_.size()) - 1);
        } else if (key.states().test(KeyState::Ctrl) && !key.states().test(KeyState::Alt) &&
                   !key.states().test(KeyState::Super) && !key.states().test(KeyState::Shift) &&
                   (key.sym() == FcitxKey_c || key.sym() == FcitxKey_C))
            RowAction(detail_selected_, detail_->revision, false);
        else if (key.check(FcitxKey_Left) || key.check(FcitxKey_Right)) {
        } else
            handled = false;
        if (handled) {
            Publish();
            event.filterAndAccept();
            return true;
        }
        ClearDetail();
    }
    if (key.check(FcitxKey_Escape)) {
        if (model_.query().empty())
            Close();
        else {
            model_.set_query("");
            if (model_.module() == rimes::capsule::Module::Temporary)
                ReloadHistory(0);
            Publish();
        }
        event.filterAndAccept();
        return true;
    }
    if (IsReturnKey(key)) {
        Activate();
        event.filterAndAccept();
        return true;
    }
    if (key.check(FcitxKey_Tab)) {
        const int offset = key.states().test(KeyState::Shift) ? -1 : 1;
        model_.CycleModule(offset);
        ReloadNotes();
        Publish();
        event.filterAndAccept();
        return true;
    }
    if (key.check(FcitxKey_Left) || key.check(FcitxKey_KP_Left)) {
        MoveSelection(-1);
        Publish();
        event.filterAndAccept();
        return true;
    }
    if (key.check(FcitxKey_Right) || key.check(FcitxKey_KP_Right)) {
        MoveSelection(1);
        Publish();
        event.filterAndAccept();
        return true;
    }
    if (key.check(FcitxKey_Up) || key.check(FcitxKey_Down)) {
        const auto& filters = rimes::capsule::ModuleDefinition(model_.module()).at("filters");
        int selected = 0;
        for (std::size_t i = 0; i < filters.size(); ++i)
            if (filters[i].at("id") == model_.filter())
                selected = i;
        if (!filters.empty()) {
            const int next =
                (selected + (key.check(FcitxKey_Up) ? -1 : 1) + static_cast<int>(filters.size())) %
                static_cast<int>(filters.size());
            model_.set_filter(filters[next].at("id"));
            if (model_.module() == rimes::capsule::Module::Temporary)
                ReloadHistory(0);
        }
        Publish();
        event.filterAndAccept();
        return true;
    }
    if (key.check(FcitxKey_BackSpace) || key.check(FcitxKey_Delete)) {
        if (model_.query().empty() && model_.module() == rimes::capsule::Module::Temporary) {
            HistoryAction("remove");
            event.filterAndAccept();
            return true;
        }
        if (model_.query().empty() && model_.module() == rimes::capsule::Module::Capture) {
            CaptureAction("remove");
            event.filterAndAccept();
            return true;
        }
        model_.backspace_query();
        if (model_.module() == rimes::capsule::Module::Temporary)
            ReloadHistory(0);
        Publish();
        event.filterAndAccept();
        return true;
    }
    if (key.states().test(KeyState::Ctrl) && !key.states().test(KeyState::Alt) &&
        !key.states().test(KeyState::Super)) {
        const auto sym = key.sym();
        if (!key.states().test(KeyState::Shift) && (sym == FcitxKey_c || sym == FcitxKey_C)) {
            CopySelected();
            event.filterAndAccept();
            return true;
        }
        int digit = -1;
        if (sym >= FcitxKey_1 && sym <= FcitxKey_9) {
            digit = static_cast<int>(sym - FcitxKey_1);
        } else if (sym >= FcitxKey_KP_1 && sym <= FcitxKey_KP_9) {
            digit = static_cast<int>(sym - FcitxKey_KP_1);
        }
        if (digit >= 0) {
            SelectIndex(digit + (model_.module() == rimes::capsule::Module::Temporary
                                     ? history_offset_
                                     : 0));
            Activate();
            event.filterAndAccept();
            return true;
        }
    }
    char printable = 0;
    if (IsPrintableAscii(event, &printable)) {
        model_.append_query(printable);
        if (model_.module() == rimes::capsule::Module::Temporary)
            ReloadHistory(0);
        Publish();
        event.filterAndAccept();
        return true;
    }
    return false;
}

void CapsuleService::OnActivate(InputContext* ic) {
    if (ic == nullptr) {
        return;
    }
    OnPasswordField(ic, ProtectedInput(ic->capabilityFlags()));
    const auto token = TokenFor(ic);
    if (!model_.visible()) {
        return;
    }
    if (!pending_unfocus_token_.empty() && pending_unfocus_token_ == token) {
        pending_unfocus_token_.clear();
        CancelFocusGraceTimer();
        // Same-IC reactivation is a field switch. Do not keep the session.
        DropSession("same-ic");
        return;
    }
    if (model_.armed() && model_.token() != token) {
        DropSession("other-ic");
        return;
    }
    if (model_.armed() && model_.token() == token) {
        return;
    }
}

void CapsuleService::OnDeactivate(InputContext* ic, bool switching_im) {
    // Fcitx disables the IM on CapabilityFlag::Password (switch-IM
    // deactivate). Mark the field before Close() so the snapshot stays
    // honest even though later keys never reach HandleEarlyKey.
    if (ic != nullptr) {
        OnPasswordField(ic, ProtectedInput(ic->capabilityFlags()));
    }
    if (switching_im && !model_.managing()) {
        Close();
        return;
    }
    if (ic == nullptr || !model_.visible()) {
        return;
    }
    if (model_.arms(TokenFor(ic))) {
        pending_unfocus_token_ = TokenFor(ic);
        ArmFocusGraceTimer();
    }
}

void CapsuleService::OnInputContextDestroyed(InputContext* ic) {
    if (ic == nullptr) {
        return;
    }
    if (model_.arms(TokenFor(ic)) || pending_unfocus_token_ == TokenFor(ic)) {
        DropSession("destroyed");
    }
}

void CapsuleService::ArmFocusGraceTimer() {
    if (instance_ == nullptr) {
        return;
    }
    focus_grace_timer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + focus_grace_ms_ * 1000, 0,
        [this](EventSourceTime*, uint64_t) {
            if (!pending_unfocus_token_.empty()) {
                DropSession("focus-grace");
            }
            return true;
        });
}

void CapsuleService::CancelFocusGraceTimer() {
    focus_grace_timer_.reset();
}

void CapsuleService::HandleCommand(const rimes::capsule::Command& command) {
    if (model_.password_field() && command.op != rimes::capsule::CommandOp::Hello &&
        command.op != rimes::capsule::CommandOp::Status && command.op != rimes::capsule::CommandOp::Close) {
        Publish(); return;
    }
    InputContext* ic = LiveTarget();
    if (ic == nullptr && instance_) {
        instance_->inputContextManager().foreachFocused([&](InputContext* focused) {
            ic = focused;
            return false;
        });
    }
    switch (command.op) {
        case rimes::capsule::CommandOp::Hello:
            break;
        case rimes::capsule::CommandOp::Status:
        case rimes::capsule::CommandOp::Refresh:
            if (command.text != "clipboard" || model_.module() == rimes::capsule::Module::Temporary)
                ReloadNotes(command.id);
            break;
        case rimes::capsule::CommandOp::Toggle:
            Toggle(ic);
            break;
        case rimes::capsule::CommandOp::Show:
            Show(ic);
            break;
        case rimes::capsule::CommandOp::Close:
            Close();
            break;
        case rimes::capsule::CommandOp::Next:
            MoveSelection(1);
            break;
        case rimes::capsule::CommandOp::Prev:
            MoveSelection(-1);
            break;
        case rimes::capsule::CommandOp::Select:
            SelectIdentity(command);
            break;
        case rimes::capsule::CommandOp::Tab:
            model_.set_tab(rimes::capsule::TabFromRaw(command.text));
            ReloadNotes();
            break;
        case rimes::capsule::CommandOp::Activate:
            if (SelectIdentity(command)) Activate();
            break;
        case rimes::capsule::CommandOp::Copy:
            if (SelectIdentity(command)) CopySelected();
            break;
        case rimes::capsule::CommandOp::Search:
            model_.set_query(command.text);
            if (model_.module() == rimes::capsule::Module::Temporary)
                ReloadHistory(0);
            break;
        case rimes::capsule::CommandOp::Module:
            if (const auto module = rimes::capsule::ModuleFromRaw(command.text)) model_.set_module(*module);
            ReloadNotes();
            break;
        case rimes::capsule::CommandOp::Filter:
            model_.set_filter(command.text);
            if (model_.module() == rimes::capsule::Module::Temporary)
                ReloadHistory(0);
            break;
        case rimes::capsule::CommandOp::Manage:
            Manage();
            break;
        case rimes::capsule::CommandOp::Rail:
            ReturnToRail();
            break;
        case rimes::capsule::CommandOp::Detail:
        if (SelectIdentity(command))
            OpenDetail();
        break;
    case rimes::capsule::CommandOp::DetailClose:
        ClearDetail();
        Publish();
        break;
    case rimes::capsule::CommandOp::RowSelect:
    case rimes::capsule::CommandOp::RowCopy:
    case rimes::capsule::CommandOp::RowInsert:
        if (detail_ && detail_->id == command.id && detail_->revision == command.revision &&
            command.index >= 0 && command.index < static_cast<int>(detail_rows_.size())) {
            if (command.op == rimes::capsule::CommandOp::RowSelect) {
                detail_selected_ = command.index;
                Publish();
            } else
                RowAction(command.index, command.revision,
                          command.op == rimes::capsule::CommandOp::RowInsert);
        }
        break;
    case rimes::capsule::CommandOp::Favorite:
    case rimes::capsule::CommandOp::Remove:
        if (model_.module()==rimes::capsule::Module::Capture) {
            if(SelectIdentity(command)) CaptureAction(command.op==rimes::capsule::CommandOp::Favorite?"favorite":"remove");
            break;
        }
        if (SelectIdentity(command))
            HistoryAction(command.op == rimes::capsule::CommandOp::Favorite ? "favorite"
                                                                            : "remove");
        break;
    case rimes::capsule::CommandOp::ClearHistory:
        HistoryAction("clear");
        break;
    case rimes::capsule::CommandOp::Unknown:
            break;
    }
    Publish();
}

void CapsuleService::EnsureUi() {
    if (headless_ || !WantsUi()) {
        return;
    }
    ReapUi();
    if (ui_pid_ > 0) {
        return;
    }
    const auto binary = FindUiBinary();
    if (binary.empty()) {
        FCITX_LOGC(rimes_capsule_log, Info) << "rimes-capsule UI is not installed";
        return;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        execl(binary.c_str(), "rimes-capsule", "--socket", socket_path_.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    ui_pid_ = pid;
}

void CapsuleService::ReapUi() {
    if (ui_pid_ <= 0) {
        return;
    }
    if (rimes::buffer::UiProcessGone(ui_pid_)) {
        ui_pid_ = 0;
    }
}

void CapsuleService::ReplaceDroppedUi() {
    if (!rimes::buffer::ShouldForceUiRespawn(ui_pid_, dropped_ui_pid_)) {
        return;
    }
    rimes::buffer::DiscardUiProcess(ui_pid_);
    ui_pid_ = 0;
}

int CapsuleService::UiClientCount() {
    std::lock_guard<std::recursive_mutex> lock(clients_mu_);
    return static_cast<int>(clients_.size());
}

void CapsuleService::StartSocket() {
    unlink(socket_path_.c_str());
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        FCITX_LOGC(rimes_capsule_log, Error) << "capsule socket() failed";
        return;
    }
    SetCloexec(listen_fd_);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(address.sun_path)) {
        close(listen_fd_);
        listen_fd_ = -1;
        return;
    }
    std::strncpy(address.sun_path, socket_path_.c_str(), sizeof(address.sun_path) - 1);
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listen_fd_, 8) != 0) {
        FCITX_LOGC(rimes_capsule_log, Error) << "capsule bind/listen failed: " << socket_path_;
        close(listen_fd_);
        listen_fd_ = -1;
        return;
    }
    chmod(socket_path_.c_str(), 0600);
    watch_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    running_.store(true);
    socket_thread_ = std::thread([this] { SocketLoop(); });
}

void CapsuleService::StopSocket() {
    running_.store(false);
    if (wake_fd_ >= 0) {
        const std::uint64_t wake = 1;
        while (write(wake_fd_, &wake, sizeof(wake)) < 0 && errno == EINTR) {}
    }
    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
    }
    {
        std::lock_guard<std::recursive_mutex> lock(clients_mu_);
        for (auto& client : clients_) {
            if (client.fd >= 0) {
                shutdown(client.fd, SHUT_RDWR);
            }
        }
    }
    if (socket_thread_.joinable()) {
        socket_thread_.join();
    }
    {
        std::lock_guard<std::recursive_mutex> lock(clients_mu_);
        for (auto& client : clients_) {
            if (client.fd >= 0) {
                close(client.fd);
            }
        }
        clients_.clear();
    }
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    if (watch_fd_ >= 0)
        close(watch_fd_);
    watch_fd_ = -1;
    watch_directory_ = -1;
    if (wake_fd_ >= 0)
        close(wake_fd_);
    wake_fd_ = -1;
    unlink(socket_path_.c_str());
}

void CapsuleService::SocketLoop() {
    while (running_.load()) {
        if (watch_fd_ >= 0 && watch_directory_ < 0)
            watch_directory_ =
                inotify_add_watch(watch_fd_, (store_.root() / "entries").c_str(),
                                  IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE |
                                      IN_CREATE | IN_DELETE_SELF | IN_MOVE_SELF);
        std::vector<pollfd> fds;
        {
            std::lock_guard<std::recursive_mutex> lock(clients_mu_);
            if (listen_fd_ >= 0) {
                fds.push_back(pollfd{listen_fd_, POLLIN, 0});
            }
            for (const auto& client : clients_) {
                fds.push_back(pollfd{client.fd,
                    static_cast<short>(POLLIN | (client.outgoing.Pending() ? POLLOUT : 0)), 0});
            }
        }
        if (watch_fd_ >= 0)
            fds.push_back(pollfd{watch_fd_, POLLIN, 0});
        if (wake_fd_ >= 0)
            fds.push_back(pollfd{wake_fd_, POLLIN, 0});
        if (fds.empty()) {
            break;
        }
        const int ready = poll(fds.data(), fds.size(), 250);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
            continue;
        }
        if (fds[0].fd == listen_fd_ && (fds[0].revents & POLLIN) != 0) {
            AcceptClient();
        }
        for (const auto& item : fds)
            if (item.fd == wake_fd_ && (item.revents & POLLIN)) {
                std::uint64_t count;
                while (read(wake_fd_, &count, sizeof(count)) < 0 && errno == EINTR) {}
            }
        for (const auto& item : fds)
            if (item.fd == watch_fd_ && (item.revents & POLLIN))
                WatchStorage();
        for (const auto& item : fds) {
            if (!(item.revents & POLLOUT))
                continue;
            std::lock_guard<std::recursive_mutex> lock(clients_mu_);
            for (auto& client : clients_)
                if (client.fd == item.fd) {
                    if (!client.outgoing.Flush(client.fd))
                        CloseClient(client.fd);
                    break;
                }
        }
        std::vector<int> readable;
        {
            std::lock_guard<std::recursive_mutex> lock(clients_mu_);
            for (const auto& item : fds) {
                if (item.fd == listen_fd_ || item.fd == watch_fd_ || item.fd == wake_fd_) {
                    continue;
                }
                if ((item.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                    readable.push_back(item.fd);
                }
            }
        }
        for (int fd : readable) {
            std::lock_guard<std::recursive_mutex> lock(clients_mu_);
            for (auto& client : clients_) {
                if (client.fd == fd) {
                    ReadClient(&client);
                    break;
                }
            }
        }
    }
}

void CapsuleService::WatchStorage() {
    alignas(inotify_event) char buffer[16384];
    bool changed = false;
    for (;;) {
        const auto count = read(watch_fd_, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        for (std::size_t at = 0; at + sizeof(inotify_event) <= static_cast<std::size_t>(count);) {
            const auto* event = reinterpret_cast<const inotify_event*>(buffer + at);
            if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) {
                if (watch_directory_ >= 0)
                    inotify_rm_watch(watch_fd_, watch_directory_);
                watch_directory_ = -1;
                changed = true;
            }
            if ((event->mask & IN_Q_OVERFLOW) ||
                (event->len && std::filesystem::path(event->name).extension() == ".md"))
                changed = true;
            at += sizeof(inotify_event) + event->len;
        }
    }
    if (changed)
        post_([this, alive = alive_] {
            if (alive->load())
                StorageChanged();
        });
}
void CapsuleService::StorageChanged() {
    if (!instance_)
        return;
    storage_timer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 100000, 0, [this](EventSourceTime*, uint64_t) {
            ReloadNotes();
            Publish();
            return true;
        });
}
void CapsuleService::AcceptClient() {
    const int fd = accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
        return;
    }
    SetCloexec(fd);
    {
        std::lock_guard<std::recursive_mutex> lock(clients_mu_);
        if (clients_.size() >= 8) {
            close(fd);
            return;
        }
        clients_.push_back(Client{fd, {}, {}});
    }
    post_([this, alive = alive_] { if (alive->load()) Publish(); });
}

void CapsuleService::ReadClient(Client* client) {
    char chunk[4096];
    const auto got = read(client->fd, chunk, sizeof(chunk));
    if (got < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        return;
    if (got <= 0) {
        CloseClient(client->fd);
        return;
    }
    client->incoming.append(chunk, static_cast<std::size_t>(got));
    while (client->incoming.size() >= 4) {
        std::uint32_t length = 0;
        if (!rimes::capsule::DecodeFrameHeader(client->incoming.data(), &length) || length > 8192) {
            CloseClient(client->fd);
            return;
        }
        if (client->incoming.size() < 4 + length) {
            return;
        }
        const std::string payload = client->incoming.substr(4, length);
        client->incoming.erase(0, 4 + length);
        rimes::capsule::Command command;
        std::string error;
        if (!rimes::capsule::ParseCommand(payload, &command, &error)) {
            WriteAll(rimes::buffer::EncodeError("bad_command", error));
            continue;
        }
        post_([this, alive = alive_, command] { if (alive->load()) HandleCommand(command); });
    }
}

void CapsuleService::CloseClient(int fd) {
    bool clients_empty = false;
    {
        std::lock_guard<std::recursive_mutex> lock(clients_mu_);
        for (auto iterator = clients_.begin(); iterator != clients_.end(); ++iterator) {
            if (iterator->fd == fd) {
                close(fd);
                clients_.erase(iterator);
                clients_empty = clients_.empty();
                break;
            }
        }
    }
    if (clients_empty && !headless_) {
        post_([this, alive = alive_] { if (alive->load()) ScheduleUiRespawn(); });
    }
}

bool CapsuleService::HasUiClients() {
    std::lock_guard<std::recursive_mutex> lock(clients_mu_);
    return !clients_.empty();
}

void CapsuleService::ScheduleUiRespawn() {
    if (headless_ || !WantsUi()) {
        return;
    }
    dropped_ui_pid_ = ui_pid_;
    ui_respawn_attempt_ = 0;
    ArmUiRespawnTimer(50000);
}

void CapsuleService::ArmUiRespawnTimer(int delay_us) {
    if (instance_ == nullptr) {
        return;
    }
    ui_respawn_timer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + delay_us, 0,
        [this](EventSourceTime*, uint64_t) {
            ReapUi();
            ReplaceDroppedUi();
            ++ui_respawn_attempt_;
            if (WantsUi()) {
                EnsureUi();
            }
            if (WantsUi() && !HasUiClients() && ui_respawn_attempt_ < 5) {
                const int delays[] = {2000000, 2000000, 2000000, 2000000};
                const int index = std::min(ui_respawn_attempt_ - 1, 3);
                ArmUiRespawnTimer(delays[index]);
            }
            return true;
        });
}

void CapsuleService::WriteAll(const std::string& payload, bool revoke) {
    std::string frame;
    if (!rimes::capsule::EncodeFrame(payload, &frame)) {
        return;
    }
    auto shared = std::make_shared<const std::string>(std::move(frame));
    std::lock_guard<std::recursive_mutex> lock(clients_mu_);
    for (auto iterator = clients_.begin(); iterator != clients_.end();) {
        if (!iterator->outgoing.Queue(shared, revoke)) {
            close(iterator->fd);
            iterator = clients_.erase(iterator);
            continue;
        }
        ++iterator;
    }
    if (wake_fd_ >= 0) {
        const std::uint64_t wake = 1;
        while (write(wake_fd_, &wake, sizeof(wake)) < 0 && errno == EINTR) {}
    }
}

void CapsuleService::Publish() {
    const auto* action_card = model_.Selected();
    const auto action_scope =
        std::string(model_.visible() ? "visible" : "hidden") + "/" +
        (model_.password_field() ? "protected" : "open") + "/" +
        rimes::capsule::ModuleRaw(model_.module()) + "/" + model_.token() + "/" +
        (action_card ? action_card->id + action_card->revision : "") + "/" +
        (model_.ModuleEnabled(model_.module()) ? "enabled" : "disabled");
    if (action_scope != history_scope_) {
        if (history_action_cancel_)
            history_action_cancel_->store(true);
        history_scope_ = action_scope;
        restore_id_.clear();
        restore_revision_.clear();
    }
    if (WantsUi()) {
        EnsureUi();
    }
    const auto* selected = model_.Selected();
    if (detail_ &&
        (!model_.visible() || model_.managing() || model_.password_field() || !selected ||
         selected->id != detail_->id || selected->revision != detail_->revision))
        ClearDetail();
    auto snapshot = rimes::capsule::MakeSnapshot(model_);
    snapshot.session_protected = session_protected_;
    snapshot.clipboard_capture = WantsClipboard();
    snapshot.restore_id = restore_id_;
    snapshot.restore_revision = restore_revision_;
    snapshot.restore_seq = restore_seq_;
    snapshot.restore_close = restore_close_;
    if (model_.module() == rimes::capsule::Module::Temporary) {
        snapshot.offset += history_offset_;
        snapshot.count = history_count_;
        if (!history_error_.empty())
            snapshot.hint = history_error_;
        else if (!model_.loading())
            snapshot.hint =
                model_.cards().empty()
                    ? (clipboard_capture_ ? "复制新的内容后将在此显示"
                                          : "剪贴板历史收录已关闭，可在 Capsule 设置中开启")
                    : "↩ INSERT / RESTORE   Ctrl+C COPY   ESC CLOSE";
    }
    if (detail_) {
        snapshot.detail = true;
        snapshot.detail_notice = detail_->format_issue;
        snapshot.detail_revision = detail_->revision;
        snapshot.detail_selected = detail_selected_;
        snapshot.detail_count = static_cast<int>(detail_rows_.size());
        snapshot.detail_offset = std::max(0, detail_selected_ - 100);
        const int end = std::min(snapshot.detail_count, snapshot.detail_offset + 200);
        for (int index = snapshot.detail_offset; index < end; ++index) {
            auto row = detail_rows_[index];
            row.label = rimes::capsule::PreviewOf(row.label);
            if (row.type == "code" && row.value.find('\n') != row.value.npos) {
                const auto count = std::count(row.value.begin(), row.value.end(), '\n') + 1;
                row.value = rimes::capsule::PreviewOf(row.value.substr(0, row.value.find('\n'))) +
                            "  …（" + std::to_string(count) + " 行）";
            } else
                row.value = rimes::capsule::PreviewOf(row.value);
            snapshot.detail_rows.push_back(std::move(row));
            snapshot.detail_headings.push_back(rimes::capsule::PreviewOf(detail_headings_[index]));
        }
        snapshot.hint = "↑ ↓ SELECT   ↩ INSERT ROW   Ctrl+C COPY ROW   "
                        "DOUBLE-CLICK INSERT   ESC BACK";
    }
    if (model_.module()==rimes::capsule::Module::Capture)
        snapshot.hint = !capture_error_.empty() ? capture_error_ : "↩ PREVIEW   Ctrl+C COPY   DELETE REMOVE TEMPORARY   ESC CLOSE";
    if(model_.module()==rimes::capsule::Module::Passwords)
        snapshot.hint="↩ UNLOCK DETAILS   ESC CLOSE · 密码不会自动上屏";
    snapshot.theme = theme_;
    snapshot.ui_clients = UiClientCount();
    const auto json = rimes::capsule::EncodeSnapshot(snapshot);
    std::string scope = snapshot.module;
    for (auto module : snapshot.modules)
        scope += std::string("/") + rimes::capsule::ModuleRaw(module);
    const bool revoke = !snapshot.visible || snapshot.password_field || scope != output_scope_ ||
                        (output_has_copy_ && snapshot.last_copied.empty());
    output_scope_ = scope;
    output_has_copy_ = !snapshot.last_copied.empty();
    WriteAll(json, revoke);
    if (dump_path_.empty()) {
        return;
    }
    const auto tmp = dump_path_ + ".tmp";
    FILE* file = std::fopen(tmp.c_str(), "w");
    if (file == nullptr) {
        return;
    }
    std::fwrite(json.data(), 1, json.size(), file);
    std::fputc('\n', file);
    std::fclose(file);
    std::rename(tmp.c_str(), dump_path_.c_str());
}

}  // namespace fcitx
