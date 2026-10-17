#include "background_jobs.hpp"
#include "capsule_password.hpp"
#include "capsule_password_view.hpp"
#include "capsule_structure.hpp"
#include "capsule_text.hpp"
#include "clipboard_desktop.hpp"
#include "desktop.hpp"
#include "official_catalog.hpp"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <set>

namespace rimes::capsule {
namespace {
using Clock = std::chrono::steady_clock;
using namespace rimes::desktop;
void Post(std::function<void()> fn, int priority = G_PRIORITY_DEFAULT) {
    g_idle_add_full(
        priority,
        +[](gpointer p) -> gboolean {
            (*static_cast<std::function<void()>*>(p))();
            return G_SOURCE_REMOVE;
        },
        new std::function<void()>(std::move(fn)),
        +[](gpointer p) { delete static_cast<std::function<void()>*>(p); });
}
void Hideable(GtkWidget* widget) {
    gtk_widget_show_all(widget);
    gtk_widget_set_no_show_all(widget, TRUE);
    gtk_widget_hide(widget);
}
GtkWidget* Scroll(GtkWidget* child) {
    auto* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), child);
    return scroll;
}
std::string Text(GtkWidget* view) {
    auto* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkTextIter start{}, end{};
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    auto* text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    std::string result = text;
    if (text) {
        std::fill(text, text + result.size(), '\0');
        g_free(text);
    }
    return result;
}
void WipeCode(RevealCode& code) {
    for (auto& chord : code)
        EraseSecret(chord);
}
} // namespace
struct PasswordView::Impl {
    enum class Purpose { View, Create, Migrate, Change, Reset, NewCode, ConfirmCode };
    GtkWindow* window;
    Send send;
    const bool preview;
    GtkWidget *root, *title, *summary, *category, *challenge, *slots, *capture, *feedback, *notice,
        *details, *rows_box, *plaintext, *plaintext_heading, *text, *actions, *edit, *save, *remove,
        *lock, *settings, *feedback_icon, *retry, *credential_toggle;
    std::vector<GtkWidget*> value_labels;
    std::vector<DetailRow> rows;
    std::vector<std::optional<TotpParameters>> totps;
    PasswordSummary metadata;
    std::optional<PasswordRecord> record;
    Snapshot state;
    std::string identity, credential, grant, migration_id, migration_revision;
    SecretAccess access;
    std::unique_ptr<plugins::PackageStore> packages;
    std::unique_ptr<DesktopClipboard> clipboard;
    runtime::BackgroundJobs jobs;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
    std::mutex commit;
    RevealCode attempt{}, old_code{}, new_code{};
    std::set<char> held;
    std::string chord;
    Purpose purpose = Purpose::View;
    int slot = 0, selected = 0, row_start = 0;
    bool busy = false, challenge_active = false, new_record = false, editing = false,
         poll_pending = false;
    Clock::time_point lease_until{}, edit_until{}, next_poll{}, delete_until{};
    guint timer = 0;
    static constexpr int page_size = 32;

    Impl(GtkWindow* parent, Send callback, bool fixture)
        : window(parent), send(std::move(callback)), preview(fixture) {
        root = Class(Column(10), "capsule-password-form");
        gtk_container_set_border_width(GTK_CONTAINER(root), 16);
        lock = Button("隐藏明文", [this] { Conceal(); }, "capsule-link");
        auto field = [&](const char* name, GtkWidget* input) {
            auto* column = Column(5);
            Add(column, Label(name, "capsule-field-label"));
            Add(column, input);
            Add(root, column);
        };
        title = Class(gtk_entry_new(), "capsule-field");
        gtk_entry_set_placeholder_text(GTK_ENTRY(title), "标题");
        field("标题", title);
        summary = Class(gtk_entry_new(), "capsule-field");
        gtk_entry_set_placeholder_text(GTK_ENTRY(summary),
                                       "一句话说明，显示在卡片上（不要写密码）");
        field("摘要", summary);
        category = gtk_combo_box_text_new();
        for (auto name : {"登录", "密钥", "其他"})
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(category), name);
        field("密码类型", category);
        challenge = Class(Column(6), "capsule-passcode");
        gtk_widget_set_margin_start(challenge, 8);
        gtk_widget_set_margin_end(challenge, 8);
        gtk_widget_set_margin_top(challenge, 5);
        gtk_widget_set_margin_bottom(challenge, 5);
        slots = Row(8);
        gtk_box_set_homogeneous(GTK_BOX(slots), TRUE);
        Add(challenge, slots);
        capture = Class(gtk_event_box_new(), "capsule-passcode-capture");
        gtk_widget_set_can_focus(capture, TRUE);
        gtk_widget_add_events(capture,
                              GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
        feedback = Label("依次按四组并击键 · Esc 返回", "capsule-writing-hint");
        gtk_label_set_line_wrap(GTK_LABEL(feedback), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(feedback), 1);
        auto* capture_content = Row(0);
        feedback_icon = Icon("lock", 12);
        gtk_widget_set_halign(feedback_icon, GTK_ALIGN_CENTER);
        Add(capture_content, feedback_icon, true);
        Add(capture_content, feedback, true);
        gtk_container_add(GTK_CONTAINER(capture), capture_content);
        gtk_widget_set_tooltip_text(capture, "依次按四组并击键；Esc 重新输入");
        gtk_widget_set_size_request(capture, -1, 30);
        Add(challenge, capture);
        retry = Button(
            "重新输入",
            [this] {
                if (Allowed())
                    Begin(purpose);
            },
            "capsule-link");
        Add(challenge, retry);
        Add(root, challenge);
        Hideable(challenge);
        details = Column(6);
        rows_box = Column(3);
        Add(details, Scroll(rows_box), true);
        auto* page = Row(8);
        Add(page, Button("上一页", [this] { Move(-page_size); }, "capsule-link"));
        Add(page, Button("下一页", [this] { Move(page_size); }, "capsule-link"));
        Add(details, page);
        Add(root, details, true);
        Hideable(details);
        plaintext = Column(5);
        plaintext_heading = Label("密码内容 · 15 秒后隐藏", "capsule-field-label");
        Add(plaintext, plaintext_heading);
        text = gtk_text_view_new();
        // Keep secret keystrokes and surrounding text outside external IM services.
        // Also prevents our own Fcitx password-field guard concealing this editor.
        g_object_set(text, "im-module", "gtk-im-context-simple", nullptr);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(text), 12);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(text), 12);
        gtk_text_view_set_top_margin(GTK_TEXT_VIEW(text), 10);
        gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(text), 10);
        gtk_text_view_set_input_purpose(GTK_TEXT_VIEW(text), GTK_INPUT_PURPOSE_PASSWORD);
        gtk_text_view_set_input_hints(
            GTK_TEXT_VIEW(text),
            static_cast<GtkInputHints>(GTK_INPUT_HINT_NO_SPELLCHECK | GTK_INPUT_HINT_NO_EMOJI));
        auto* area = Class(Scroll(text), "capsule-text");
        gtk_widget_set_size_request(area, -1, 150);
        Add(plaintext, area, true);
        Add(plaintext, Label("「- 标签：值」是字段；「## 标题」分组。", "capsule-writing-hint"));
        Add(root, plaintext, true);
        Hideable(plaintext);
        actions = Row(8);
        edit = Button("编辑明文", [this] { Edit(); }, "capsule-link");
        remove = Button("删除", [this] { Remove(); }, "capsule-link");
        Add(actions, remove);
        save = Button("保存", [this] { Save(); });
        Add(actions, save);
        notice = Label("1 条本地记录", "capsule-status");
        gtk_label_set_line_wrap(GTK_LABEL(notice), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(notice), 1);
        settings = Row(8);
        Add(settings, Button(
                          "更改查看口令…",
                          [this] {
                              Conceal();
                              Begin(Purpose::Change);
                          },
                          "capsule-link"));
        Add(settings, Button(
                          "恢复默认口令…",
                          [this] {
                              Conceal();
                              Begin(Purpose::Reset);
                          },
                          "capsule-link"));
        Hideable(settings);
        Add(root, settings);
        Add(root, gtk_label_new(""), true);
        auto* controls = Row(8);
        Add(controls, lock);
        Add(controls, edit);
        credential_toggle = Button(
            "查看口令…",
            [this] { gtk_widget_set_visible(settings, !gtk_widget_get_visible(settings)); },
            "capsule-link");
        Add(controls, credential_toggle);
        Add(root, controls);
        Add(actions, notice, true);
        gtk_box_reorder_child(GTK_BOX(actions), notice, 0);
        Add(root, actions);
        for (auto* widget : {lock, edit, retry, feedback, feedback_icon})
            gtk_widget_set_no_show_all(widget, TRUE);
        for (auto item :
             std::vector<std::pair<GtkWidget*, const char*>>{{root, "capsule-password-form"},
                                                             {title, "vault-title"},
                                                             {summary, "vault-summary"},
                                                             {category, "vault-category"},
                                                             {capture, "vault-chord-input"},
                                                             {slots, "vault-chord-slots"},
                                                             {text, "vault-plaintext"},
                                                             {notice, "vault-status"},
                                                             {edit, "vault-edit"},
                                                             {save, "vault-save"},
                                                             {remove, "vault-remove"},
                                                             {lock, "vault-lock"},
                                                             {rows_box, "vault-rows"}})
            gtk_widget_set_name(item.first, item.second);
        g_signal_connect(
            capture, "button-press-event",
            G_CALLBACK(+[](GtkWidget* widget, GdkEventButton*, gpointer raw) -> gboolean {
                auto* self = static_cast<Impl*>(raw);
                if (self->Allowed() && !self->busy) {
                    self->challenge_active = true;
                    gtk_widget_grab_focus(widget);
                }
                return TRUE;
            }),
            this);
        g_signal_connect(window, "key-press-event",
                         G_CALLBACK(+[](GtkWidget*, GdkEventKey* event, gpointer raw) -> gboolean {
                             return static_cast<Impl*>(raw)->Key(event, true);
                         }),
                         this);
        g_signal_connect(window, "key-release-event",
                         G_CALLBACK(+[](GtkWidget*, GdkEventKey* event, gpointer raw) -> gboolean {
                             return static_cast<Impl*>(raw)->Key(event, false);
                         }),
                         this);
        g_signal_connect(window, "focus-out-event",
                         G_CALLBACK(+[](GtkWidget*, GdkEventFocus*, gpointer raw) -> gboolean {
                             static_cast<Impl*>(raw)->CheckFocusSoon();
                             return FALSE;
                         }),
                         this);
        g_signal_connect(category, "notify::popup-shown",
                         G_CALLBACK(+[](GObject*, GParamSpec*, gpointer raw) {
                             static_cast<Impl*>(raw)->CheckFocusSoon();
                         }),
                         this);
        g_signal_connect(text, "realize", G_CALLBACK(+[](GtkWidget* widget, gpointer) {
                             // Selecting text must never put a password in the PRIMARY selection.
                             g_object_set_data(G_OBJECT(widget), "vault-primary-detached",
                                               GINT_TO_POINTER(1));
                             gtk_text_buffer_remove_selection_clipboard(
                                 gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget)),
                                 gtk_clipboard_get(GDK_SELECTION_PRIMARY));
                         }),
                         nullptr);
        auto restore_primary = +[](GtkWidget* widget, gpointer) {
            if (!g_object_get_data(G_OBJECT(widget), "vault-primary-detached"))
                return;
            g_object_set_data(G_OBJECT(widget), "vault-primary-detached", nullptr);
            auto* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget));
            gtk_text_buffer_set_text(buffer, "", 0);
            gtk_text_buffer_add_selection_clipboard(buffer,
                                                    gtk_clipboard_get(GDK_SELECTION_PRIMARY));
        };
        g_signal_connect(text, "unrealize", G_CALLBACK(restore_primary), nullptr);
        g_signal_connect(text, "destroy", G_CALLBACK(restore_primary), nullptr);
        g_signal_connect(
            text, "drag-data-get",
            G_CALLBACK(+[](GtkWidget* widget, GdkDragContext*, GtkSelectionData*, guint, guint,
                           gpointer) { g_signal_stop_emission_by_name(widget, "drag-data-get"); }),
            nullptr);
        gtk_drag_dest_unset(text);
        g_signal_connect(text, "drag-data-received",
                         G_CALLBACK(+[](GtkWidget* widget, GdkDragContext* context, gint, gint,
                                        GtkSelectionData*, guint, guint time, gpointer) {
                             g_signal_stop_emission_by_name(widget, "drag-data-received");
                             gtk_drag_finish(context, FALSE, FALSE, time);
                         }),
                         nullptr);
        g_signal_connect(text, "paste-clipboard",
                         G_CALLBACK(+[](GtkTextView* widget, gpointer raw) {
                             g_signal_stop_emission_by_name(widget, "paste-clipboard");
                             static_cast<Impl*>(raw)->Paste();
                         }),
                         this);
        g_signal_connect(text, "copy-clipboard", G_CALLBACK(+[](GtkTextView* view, gpointer raw) {
                             g_signal_stop_emission_by_name(view, "copy-clipboard");
                             static_cast<Impl*>(raw)->CopySelection(false);
                         }),
                         this);
        g_signal_connect(text, "cut-clipboard", G_CALLBACK(+[](GtkTextView* view, gpointer raw) {
                             g_signal_stop_emission_by_name(view, "cut-clipboard");
                             static_cast<Impl*>(raw)->CopySelection(true);
                         }),
                         this);
        if (!preview) {
            packages = std::make_unique<plugins::PackageStore>(
                runtime::UserRoot() / "plugins", plugins::OfficialCatalog(),
                plugins::BundledPackages(), plugins::CompiledAdapters(), RIMES_PRODUCT_VERSION);
            clipboard = std::make_unique<DesktopClipboard>([](auto, auto, auto) {}, [](auto) {});
        }
        timer = g_timeout_add(
            200,
            +[](gpointer raw) -> gboolean {
                static_cast<Impl*>(raw)->Tick();
                return G_SOURCE_CONTINUE;
            },
            this);
        Controls();
        Slots();
    }
    ~Impl() {
        alive->store(false);
        Conceal();
        if (timer)
            g_source_remove(timer);
        g_signal_handlers_disconnect_by_data(window, this);
        jobs.Shutdown();
        clipboard.reset();
    }
    bool Allowed() const {
        return !preview && state.visible && state.managing && !state.password_field &&
               state.module == "passwords" &&
               std::find(state.modules.begin(), state.modules.end(), Module::Passwords) !=
                   state.modules.end();
    }
    bool OwnedCategoryPopup() const {
        gboolean shown = FALSE;
        g_object_get(category, "popup-shown", &shown, nullptr);
        auto* grab = gtk_grab_get_current();
        return shown && grab && GTK_IS_MENU(grab) &&
               gtk_menu_get_attach_widget(GTK_MENU(grab)) == category;
    }
    bool Focused() const {
        return gtk_widget_get_visible(GTK_WIDGET(window)) &&
               (gtk_window_is_active(window) || OwnedCategoryPopup());
    }
    void CheckFocusSoon() {
        // GTK transfers keyboard focus to the combo's own menu. Let that one
        // event finish before checking ownership; any external focus still
        // revokes the lease, including when an open menu is dismissed.
        const auto life = alive;
        Post(
            [this, life] {
                if (life->load() && !Focused())
                    Conceal();
            },
            G_PRIORITY_DEFAULT_IDLE);
    }
    bool Valid() const {
        return Allowed() && Focused() && record &&
               (new_record ||
                access.Valid(metadata.id, metadata.revision, credential, Clock::now()));
    }
    SecretCommit Gate(const std::shared_ptr<std::atomic<bool>>& token,
                      Clock::time_point until = Clock::time_point::max()) {
        return [this, life = alive, token, until](const std::function<void()>& action) {
            std::lock_guard<std::mutex> guard(commit);
            if (!token->load() && life->load() && Clock::now() < until)
                action();
        };
    }
    void Notice(const std::string& message) {
        gtk_label_set_text(GTK_LABEL(notice), message.c_str());
    }
    void Slots() {
        Clear(slots);
        for (int i = 0; i < 4; ++i) {
            auto* label = Label(i < slot ? "•" : std::to_string(i + 1), "capsule-passcode-slot");
            gtk_widget_set_size_request(label, 24, 23);
            gtk_label_set_xalign(GTK_LABEL(label), 0.5);
            if (i == std::min(slot, 3))
                Class(label, "current");
            Add(slots, label, true);
        }
        gtk_widget_show_all(slots);
    }
    void ResetAttempt() {
        held.clear();
        EraseSecret(chord);
        WipeCode(attempt);
        slot = 0;
        Slots();
    }
    void FillMetadata() {
        gtk_entry_set_text(GTK_ENTRY(title), metadata.title.c_str());
        gtk_entry_set_text(GTK_ENTRY(summary), metadata.summary.c_str());
        gtk_combo_box_set_active(GTK_COMBO_BOX(category), metadata.category == "login" ? 0
                                                          : metadata.category == "key" ? 1
                                                                                       : 2);
    }
    void Controls() {
        const bool valid = Valid();
        gtk_editable_set_editable(GTK_EDITABLE(title), valid && !busy);
        gtk_editable_set_editable(GTK_EDITABLE(summary), valid && !busy);
        gtk_widget_set_sensitive(category, valid && !busy);
        const bool surface =
            state.visible && state.managing && !state.password_field && state.module == "passwords";
        gtk_widget_set_visible(challenge, surface && !valid);
        gtk_widget_set_visible(details, valid && !editing);
        gtk_widget_set_visible(plaintext, valid && editing);
        gtk_widget_set_sensitive(edit, valid && !editing && !busy);
        gtk_widget_set_sensitive(remove, valid && !new_record && !busy);
        gtk_widget_set_sensitive(save, valid && !busy);
        gtk_widget_set_sensitive(lock, valid || challenge_active);
        gtk_widget_set_sensitive(settings, Allowed() && !busy);
        gtk_widget_set_sensitive(capture, Allowed() && !busy);
        gtk_widget_set_visible(edit, valid && !editing);
        gtk_widget_set_visible(lock, valid);
        const bool instructions = purpose != Purpose::View || slot > 0;
        gtk_widget_set_visible(feedback, instructions);
        gtk_widget_set_visible(feedback_icon, !instructions);
        gtk_widget_set_visible(retry, slot > 0);
        gtk_button_set_label(GTK_BUTTON(save), new_record ? "创建" : "保存");
    }
    void Conceal() {
        {
            std::lock_guard<std::mutex> guard(commit);
            cancelled->store(true);
            cancelled = std::make_shared<std::atomic<bool>>(false);
        }
        jobs.CancelAll();
        busy = false;
        poll_pending = false;
        access.Conceal();
        record.reset();
        for (auto& row : rows) {
            EraseSecret(row.value);
            EraseSecret(row.label);
        }
        rows.clear();
        totps.clear();
        value_labels.clear();
        Clear(rows_box);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(text)), "", 0);
        editing = false;
        new_record = false;
        challenge_active = false;
        lease_until = {};
        edit_until = {};
        delete_until = {};
        ResetAttempt();
        WipeCode(old_code);
        WipeCode(new_code);
        credential.clear();
        grant.clear();
        gtk_button_set_label(GTK_BUTTON(remove), "删除");
        FillMetadata();
        if (clipboard)
            clipboard->CancelWrites();
        Controls();
    }
    void Apply(const Snapshot& snapshot) {
        auto next = snapshot.module + ":" + snapshot.selected_id;
        const auto found =
            std::find_if(snapshot.cards.begin(), snapshot.cards.end(),
                         [&](const auto& card) { return card.id == snapshot.selected_id; });
        const std::string revision = found == snapshot.cards.end() ? "" : found->revision;
        next += ":" + revision;
        const bool changed = next != identity;
        state = snapshot;
        if (changed || !Allowed()) {
            Conceal();
            identity = next;
            metadata = {};
            if (found != snapshot.cards.end() && found->kind == Kind::Password) {
                metadata.id = found->id;
                metadata.revision = found->revision;
                metadata.title = found->title;
                metadata.summary = found->preview == "••••••••" ? "" : found->preview;
                metadata.category = found->filter;
            }
            FillMetadata();
            if (Allowed()) {
                purpose = Purpose::View;
                Notice(std::to_string(snapshot.count) + " 条本地记录");
                Begin(Purpose::View, false);
            }
        }
        if (clipboard)
            clipboard->Configure(false,
                                 snapshot.session_protected ||
                                     std::find(snapshot.modules.begin(), snapshot.modules.end(),
                                               Module::Passwords) == snapshot.modules.end());
        if (Allowed() && !migration_id.empty() && purpose != Purpose::Migrate)
            Begin(Purpose::Migrate, false);
        if (!snapshot.visible || snapshot.password_field) {
            migration_id.clear();
            migration_revision.clear();
        }
        Controls();
    }
    void Begin(Purpose mode, bool focus = true) {
        if (!Allowed() || busy)
            return;
        purpose = mode;
        challenge_active = true;
        ResetAttempt();
        const char* message = mode == Purpose::NewCode ? "输入新的四组并击口令"
                              : mode == Purpose::ConfirmCode ? "再次输入四组并击进行确认"
                              : mode == Purpose::Change ? "先输入当前查看口令，再设置新口令"
                              : mode == Purpose::Reset ? "输入当前口令，验证后恢复默认口令"
                              : mode == Purpose::Migrate ? "输入查看口令，将笔记加密移入密码库"
                                                         : "依次按四组并击键 · Esc 返回";
        gtk_label_set_text(GTK_LABEL(feedback), message);
        Controls();
        if (focus)
            gtk_widget_grab_focus(capture);
    }
    gboolean Key(GdkEventKey* event, bool down) {
        auto* focused = gtk_window_get_focus(window);
        if (!Allowed() || !focused)
            return FALSE;
        if (focused == capture && challenge_active && !busy) {
            if (event->keyval == GDK_KEY_Escape) {
                if (down)
                    Conceal();
                return TRUE;
            }
            if (event->keyval == GDK_KEY_Shift_L || event->keyval == GDK_KEY_Shift_R)
                return TRUE;
            if (event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK | GDK_META_MASK |
                                GDK_HYPER_MASK)) {
                ResetAttempt();
                gtk_label_set_text(GTK_LABEL(feedback),
                                   "只使用字母物理键，不带 Ctrl、Alt 或 Super");
                return TRUE;
            }
            const char letter = RevealPhysicalLetter(event->hardware_keycode);
            if (!letter) {
                if (down) {
                    ResetAttempt();
                    gtk_label_set_text(GTK_LABEL(feedback),
                                       "请使用字母键并击，每组全部松开后再输入下一组");
                }
                return TRUE;
            }
            if (down) {
                if (held.insert(letter).second && chord.find(letter) == chord.npos)
                    chord += letter;
                if (chord.size() > 8) {
                    ResetAttempt();
                    gtk_label_set_text(GTK_LABEL(feedback), "每组最多八个字母键");
                }
                return TRUE;
            }
            if (!held.erase(letter) || !held.empty())
                return TRUE;
            const auto value = CanonicalChord(chord);
            EraseSecret(chord);
            if (!value)
                return TRUE;
            attempt[slot++] = *value;
            Slots();
            if (slot == 4) {
                auto code = attempt;
                ResetAttempt();
                Accept(code);
                WipeCode(code);
            }
            return TRUE;
        }
        if (down && Valid() && (event->state & GDK_CONTROL_MASK) && event->keyval == GDK_KEY_s) {
            Save();
            return TRUE;
        }
        if (!down || !Valid() || editing || !gtk_widget_is_ancestor(focused, rows_box))
            return FALSE;
        if (event->keyval == GDK_KEY_Up) {
            Move(-1);
            return TRUE;
        }
        if (event->keyval == GDK_KEY_Down) {
            Move(1);
            return TRUE;
        }
        if (event->keyval == GDK_KEY_space) {
            Reveal(selected);
            return TRUE;
        }
        if (event->keyval == GDK_KEY_Return) {
            CopyRow(selected);
            return TRUE;
        }
        return FALSE;
    }
    void Accept(RevealCode code) {
        if (purpose == Purpose::NewCode) {
            new_code = code;
            Begin(Purpose::ConfirmCode);
            return;
        }
        if (purpose == Purpose::ConfirmCode) {
            if (code != new_code) {
                WipeCode(new_code);
                Notice("两次口令不一致，请重新设置");
                Begin(Purpose::NewCode);
                return;
            }
            ChangeCredential(false);
            return;
        }
        const auto action = purpose;
        const auto life = alive, token = cancelled;
        const auto expected = metadata;
        const auto source_id = migration_id, source_revision = migration_revision;
        busy = true;
        Controls();
        Notice("正在验证…");
        auto result = std::make_shared<PasswordRecord>();
        auto revision = std::make_shared<std::string>();
        auto permission = std::make_shared<std::string>();
        auto error = std::make_shared<std::string>();
        auto work = [this, life, token, code, expected, source_id, source_revision, action, result,
                     revision, permission, error]() mutable {
            bool ok = false;
            try {
                const auto entry = packages->Inspect(ModulePlugin(Module::Passwords));
                std::vector<std::pair<std::string, std::string>> permissions{
                    {entry.id, entry.grant}};
                if (action == Purpose::Migrate) {
                    const auto note = packages->Inspect(ModulePlugin(Module::Notes));
                    permissions.emplace_back(note.id, note.grant);
                }
                packages->WithEnabledGrants(permissions, [&] {
                    if (token->load() || !life->load())
                        return;
                    RevealCredential verifier(ContentStore::DefaultRoot());
                    *revision = verifier.Revision();
                    if ((action != Purpose::Create && !verifier.Matches(code)) ||
                        verifier.Revision() != *revision)
                        throw std::runtime_error("查看口令不正确或已变化");
                    *permission = entry.grant;
                    if (action == Purpose::View) {
                        if (expected.id.empty())
                            throw std::runtime_error("请先选择密码，或点击新建");
                        *result = PasswordStore(ContentStore::DefaultRoot())
                                      .Read(expected.id, expected.revision, *revision);
                    }
                    if (action == Purpose::Migrate) {
                        if (token->load() || !life->load())
                            return;
                        const auto saved = PasswordStore(ContentStore::DefaultRoot())
                                               .MigrateNote(source_id, source_revision, "other",
                                                            *revision, Gate(token));
                        static_cast<PasswordSummary&>(*result) = saved;
                    }
                    ok = true;
                });
            } catch (const std::exception& e) {
                *error = e.what();
            }
            Post([this, life, token, action, result, revision, permission, error, ok,
                  code]() mutable {
                if (!life->load() || token->load()) {
                    WipeCode(code);
                    return;
                }
                busy = false;
                if (!Allowed() || !Focused()) {
                    Conceal();
                    WipeCode(code);
                    return;
                }
                if (!ok) {
                    Notice(error->empty() ? "授权已撤销，请重试" : *error);
                    Begin(action);
                    WipeCode(code);
                    return;
                }
                credential = *revision;
                grant = *permission;
                if (action == Purpose::Change || action == Purpose::Reset) {
                    old_code = code;
                    if (action == Purpose::Reset)
                        ChangeCredential(true);
                    else
                        Begin(Purpose::NewCode);
                    WipeCode(code);
                    return;
                }
                WipeCode(code);
                if (action == Purpose::Migrate) {
                    migration_id.clear();
                    migration_revision.clear();
                    const auto id = result->id;
                    Conceal();
                    Notice("已加密迁移；已有外部备份不会被追溯删除");
                    send(Json{{"v", 1}, {"op", "refresh"}, {"id", id}}.dump());
                    return;
                }
                record = *result;
                new_record = action == Purpose::Create;
                if (new_record) {
                    metadata = {};
                    metadata.category = "login";
                    record->category = "login";
                    record->body = "- 用户名：\n- 密码：";
                } else
                    metadata = *result;
                lease_until =
                    new_record ? Clock::time_point::max() : Clock::now() + std::chrono::seconds(60);
                access.Authorize(new_record ? "new" : metadata.id, metadata.revision, credential,
                                 Clock::now());
                challenge_active = false;
                FillMetadata();
                BuildRows();
                Notice("详情授权 60 秒；敏感字段显示 15 秒，复制后 60 秒清理");
                Controls();
                if (new_record)
                    Edit();
                else if (!value_labels.empty())
                    gtk_widget_grab_focus(
                        gtk_widget_get_parent(gtk_widget_get_parent(value_labels.front())));
            });
            WipeCode(code);
        };
        if (!jobs.Run(token, std::move(work))) {
            busy = false;
            Notice("密码库正忙，请重试");
            Controls();
        }
    }
    void ChangeCredential(bool reset) {
        if (!Allowed() || busy || credential.empty())
            return;
        busy = true;
        Controls();
        const auto life = alive, token = cancelled;
        const auto revision = credential, expected_grant = grant;
        auto current = old_code;
        auto next = reset ? std::optional<RevealCode>{} : std::make_optional(new_code);
        const bool queued = jobs.Run(token, [this, life, token, revision, expected_grant, current,
                                             next]() mutable {
            std::string error;
            bool ok = false;
            try {
                packages->WithEnabledGrant(ModulePlugin(Module::Passwords), expected_grant, [&] {
                    if (token->load() || !life->load())
                        return;
                    RevealCredential(ContentStore::DefaultRoot())
                        .Change(current, next, revision, Gate(token));
                    ok = true;
                });
            } catch (const std::exception& e) {
                error = e.what();
            }
            WipeCode(current);
            if (next)
                WipeCode(*next);
            Post([this, life, token, ok, error] {
                if (!life->load() || token->load())
                    return;
                Conceal();
                Notice(ok ? "查看口令已更新，所有旧授权已失效" : error);
                Begin(Purpose::View, false);
            });
        });
        WipeCode(current);
        if (next)
            WipeCode(*next);
        if (!queued) {
            busy = false;
            Notice("密码库正忙，请重试");
            Controls();
        }
    }
    void BuildRows() {
        for (auto& row : rows) {
            EraseSecret(row.value);
            EraseSecret(row.label);
        }
        rows.clear();
        totps.clear();
        if (record)
            rows = FlattenSections(NoteSections(record->body));
        for (auto& row : rows) {
            auto totp = ParseTotp(row.label, row.value);
            if (totp)
                row.secret = true;
            totps.push_back(std::move(totp));
        }
        selected = 0;
        RenderRows();
    }
    void RenderRows() {
        Clear(rows_box);
        value_labels.clear();
        row_start = (selected / page_size) * page_size;
        for (int i = row_start; i < std::min<int>(rows.size(), row_start + page_size); ++i) {
            auto* line = Row(6);
            auto* value = Label("", "capsule-password-value");
            gtk_label_set_xalign(GTK_LABEL(value), 0);
            gtk_label_set_ellipsize(GTK_LABEL(value), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(value), 1);
            auto* button = Button(
                "",
                [this, i] {
                    selected = i;
                    CopyRow(i);
                },
                "capsule-password-row");
            auto* column = Column(2);
            if (!rows[i].label.empty())
                Add(column, Label(rows[i].label, "capsule-field-label"));
            Add(column, value);
            gtk_widget_destroy(gtk_bin_get_child(GTK_BIN(button)));
            gtk_container_add(GTK_CONTAINER(button), column);
            Add(line, button, true);
            gtk_widget_set_name(button, ("vault-row-" + std::to_string(i)).c_str());
            if (rows[i].secret) {
                auto* eye = IconButton("eye", "查看此字段 15 秒", [this, i] {
                    selected = i;
                    Reveal(i);
                });
                gtk_widget_set_name(eye, ("vault-reveal-" + std::to_string(i)).c_str());
                Add(line, eye);
            }
            Add(rows_box, line);
            value_labels.push_back(value);
        }
        gtk_widget_show_all(rows_box);
        Values();
    }
    void Values() {
        const bool valid = Valid();
        const auto now = Clock::now();
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
        for (std::size_t at = 0; at < value_labels.size(); ++at) {
            const auto i = row_start + at;
            const auto& row = rows[i];
            std::string value = "••••••••";
            if (valid && (!row.secret || access.Revealed(i, now))) {
                if (totps[i]) {
                    const auto code = TotpCode(*totps[i], seconds);
                    value = code.first + " · " + std::to_string(code.second) + "s";
                } else
                    value = row.value;
            }
            if (value != gtk_label_get_text(GTK_LABEL(value_labels[at])))
                gtk_label_set_text(GTK_LABEL(value_labels[at]), value.c_str());
            EraseSecret(value);
        }
    }
    void Move(int delta) {
        if (!Valid() || rows.empty())
            return;
        selected = std::clamp(selected + delta, 0, int(rows.size()) - 1);
        if (selected < row_start || selected >= row_start + page_size)
            RenderRows();
        auto* row =
            gtk_widget_get_parent(gtk_widget_get_parent(value_labels[selected - row_start]));
        gtk_widget_grab_focus(row);
    }
    void Reveal(int index) {
        if (!Valid() || index < 0 || index >= int(rows.size()))
            return;
        access.Reveal(index, Clock::now());
        Values();
    }
    bool Copy(std::string value, std::function<void()> completed = {}) {
        if (!Valid() || !clipboard) {
            EraseSecret(value);
            return false;
        }
        const auto life = alive, token = cancelled;
        const auto expected_credential = credential, expected_grant = grant, id = metadata.id,
                   revision = metadata.revision;
        const auto until = lease_until;
        const bool creating = new_record;
        auto gate = [this, life, token, expected_credential, expected_grant, id, revision, until,
                     creating](const std::function<void()>& write) {
            if (!life->load() || token->load() || Clock::now() >= until)
                return;
            packages->WithEnabledGrant(
                ModulePlugin(Module::Passwords), expected_grant,
                [&] {
                    PasswordStore(ContentStore::DefaultRoot())
                        .WithAuthorization(creating ? "" : id, revision, expected_credential,
                                           [&] { Gate(token, until)(write); });
                },
                false);
        };
        bool queued = false;
        try {
            queued = clipboard->Restore(
                ClipboardArchive{{{"text/plain;charset=utf-8", value}, {"UTF8_STRING", value}}},
                true,
                [this, life, token, completed](bool success) {
                    Post([this, life, token, completed, success] {
                        if (!life->load() || token->load())
                            return;
                        Notice(success ? "已复制敏感内容，60 秒后清理本次复制"
                                       : "复制失败，授权已变化或桌面不可用");
                        if (success && completed && Valid())
                            completed();
                    });
                },
                std::move(gate));
        } catch (const std::exception&) {
        }
        EraseSecret(value);
        Notice(queued ? "正在复制敏感内容…" : "复制失败，桌面未开放剪贴板接口");
        return queued;
    }
    void CopyRow(int index) {
        if (!Valid() || index < 0 || index >= int(rows.size()))
            return;
        auto value = rows[index].value;
        if (totps[index]) {
            EraseSecret(value);
            value = TotpCode(*totps[index], std::chrono::duration_cast<std::chrono::seconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count())
                        .first;
        }
        Copy(std::move(value));
    }
    void Paste() {
        if (!Valid() || !editing)
            return;
        struct Request {
            Impl* view;
            std::shared_ptr<std::atomic<bool>> life, token;
        };
        gtk_clipboard_request_text(
            gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
            +[](GtkClipboard*, const gchar* value, gpointer data) {
                std::unique_ptr<Request> request(static_cast<Request*>(data));
                if (!request->life->load() || request->token->load() || !value)
                    return;
                auto* self = request->view;
                if (!self->Valid() || !self->editing)
                    return;
                if (g_utf8_strlen(value, -1) > 65536) {
                    self->Notice("粘贴内容超过密码正文上限");
                    return;
                }
                gtk_text_buffer_insert_at_cursor(
                    gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text)), value, -1);
            },
            new Request{this, alive, cancelled});
    }
    void CopySelection(bool cut) {
        if (!Valid() || !editing)
            return;
        auto* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text));
        GtkTextIter start{}, end{};
        if (!gtk_text_buffer_get_selection_bounds(buffer, &start, &end))
            return;
        auto* raw = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
        std::string value = raw;
        std::fill(raw, raw + value.size(), '\0');
        g_free(raw);
        const int first = gtk_text_iter_get_offset(&start), last = gtk_text_iter_get_offset(&end);
        auto body = Text(text);
        const auto revision = runtime::Sha256(body);
        EraseSecret(body);
        Copy(std::move(value), [this, cut, first, last, revision] {
            if (!cut || !editing)
                return;
            auto current = Text(text);
            const bool unchanged = runtime::Sha256(current) == revision;
            EraseSecret(current);
            if (!unchanged)
                return;
            auto* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text));
            GtkTextIter start{}, end{};
            gtk_text_buffer_get_iter_at_offset(buffer, &start, first);
            gtk_text_buffer_get_iter_at_offset(buffer, &end, last);
            gtk_text_buffer_delete(buffer, &start, &end);
        });
    }
    void Edit() {
        if (!Valid() || busy)
            return;
        editing = true;
        edit_until = new_record ? Clock::time_point::max()
                                : std::min(lease_until, Clock::now() + std::chrono::seconds(15));
        gtk_label_set_text(GTK_LABEL(plaintext_heading),
                           new_record ? "密码内容 · 保存时加密" : "密码内容 · 15 秒后隐藏");
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(text)), record->body.data(),
                                 record->body.size());
        Controls();
        gtk_widget_grab_focus(text);
        Notice(new_record ? "新建内容保存时加密；失焦会清除未保存的明文"
                          : "明文编辑 15 秒；请在隐藏前保存，超时会清除未保存修改");
    }
    void Save() {
        if (!Valid() || busy)
            return;
        auto value = std::make_shared<PasswordRecord>(*record);
        value->title = gtk_entry_get_text(GTK_ENTRY(title));
        value->summary = gtk_entry_get_text(GTK_ENTRY(summary));
        const int kind = gtk_combo_box_get_active(GTK_COMBO_BOX(category));
        value->category = kind == 0 ? "login" : kind == 1 ? "key" : "other";
        if (editing)
            value->body = Text(text);
        Write(false, value);
    }
    void Remove() {
        if (!Valid() || busy || new_record)
            return;
        if (Clock::now() >= delete_until) {
            delete_until = Clock::now() + std::chrono::seconds(5);
            gtk_button_set_label(GTK_BUTTON(remove), "再次点击确认删除");
            Notice("删除此密码记录；主密钥与其他记录会保留");
            return;
        }
        Write(true, {});
    }
    void Write(bool remove_record, std::shared_ptr<PasswordRecord> value) {
        const auto life = alive, token = cancelled;
        const auto id = metadata.id, revision = metadata.revision, expected_credential = credential,
                   expected_grant = grant;
        const auto until = lease_until;
        busy = true;
        Controls();
        if (!jobs.Run(token, [this, life, token, id, revision, expected_credential, expected_grant,
                              until, value, remove_record] {
                std::string error, saved_id;
                bool ok = false;
                try {
                    packages->WithEnabledGrant(
                        ModulePlugin(Module::Passwords), expected_grant, [&] {
                            if (token->load() || !life->load() || Clock::now() >= until)
                                return;
                            PasswordStore store(ContentStore::DefaultRoot());
                            if (remove_record)
                                store.Remove(id, revision, expected_credential, Gate(token, until));
                            else
                                saved_id = store
                                               .Put(*value, revision, expected_credential,
                                                    Gate(token, until))
                                               .id;
                            ok = true;
                        });
                } catch (const std::exception& e) {
                    error = e.what();
                }
                Post([this, life, token, ok, error, saved_id, remove_record] {
                    if (!life->load() || token->load())
                        return;
                    busy = false;
                    if (ok) {
                        Conceal();
                        Notice(remove_record ? "已删除此记录" : "已加密保存");
                        send(Json{{"v", 1}, {"op", "refresh"}, {"id", saved_id}}.dump());
                    } else {
                        Notice(error.empty() ? "授权已过期，请重新验证" : error);
                        Controls();
                    }
                });
            })) {
            busy = false;
            Notice("密码库正忙，请重试");
            Controls();
        }
    }
    void New() {
        if (!Allowed() || busy)
            return;
        Conceal();
        metadata = {};
        FillMetadata();
        purpose = Purpose::Create;
        Accept({}); // Creating user-supplied content does not unlock an old secret.
    }
    void Tick() {
        if (record && (!Valid() || (editing && Clock::now() >= edit_until))) {
            Conceal();
            Notice("已自动隐藏；未保存的明文修改已清除");
            return;
        }
        if (record)
            Values();
        if (delete_until != Clock::time_point{} && Clock::now() >= delete_until) {
            delete_until = {};
            gtk_button_set_label(GTK_BUTTON(remove), "删除");
        }
        if (!record || busy || poll_pending || Clock::now() < next_poll)
            return;
        poll_pending = true;
        next_poll = Clock::now() + std::chrono::seconds(1);
        const auto life = alive, token = cancelled;
        const auto id = metadata.id, revision = metadata.revision, expected_credential = credential,
                   expected_grant = grant;
        const bool creating = new_record;
        if (!jobs.Run(token, [this, life, token, id, revision, expected_credential, expected_grant,
                              creating] {
                bool valid = false;
                try {
                    const auto entry = packages->Inspect(ModulePlugin(Module::Passwords));
                    valid = entry.enabled && entry.grant == expected_grant &&
                            RevealCredential(ContentStore::DefaultRoot()).Revision() ==
                                expected_credential &&
                            (creating ||
                             PasswordStore(ContentStore::DefaultRoot()).Inspect(id).revision ==
                                 revision);
                } catch (const std::exception&) {
                }
                Post([this, life, token, valid] {
                    if (!life->load() || token->load())
                        return;
                    poll_pending = false;
                    if (!valid) {
                        Conceal();
                        Notice("记录、口令或插件授权已变化，请重新验证");
                        send(Json{{"v", 1}, {"op", "refresh"}}.dump());
                    }
                });
            }))
            poll_pending = false;
    }
};
PasswordView::PasswordView(GtkWindow* window, Send send, bool preview)
    : impl_(std::make_unique<Impl>(window, std::move(send), preview)) {}
PasswordView::~PasswordView() = default;
GtkWidget* PasswordView::Widget() const { return impl_->root; }
void PasswordView::Apply(const Snapshot& state) { impl_->Apply(state); }
void PasswordView::New() { impl_->New(); }
void PasswordView::PrepareMigration(std::string id, std::string revision) {
    impl_->migration_id = std::move(id);
    impl_->migration_revision = std::move(revision);
}
void PasswordView::Conceal() { impl_->Conceal(); }
} // namespace rimes::capsule
