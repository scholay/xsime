#include "capsule_history.hpp"
#include "capsule_service.hpp"
#include "official_catalog.hpp"
#include "session/protected_input.hpp"
#include <algorithm>

namespace fcitx {
using namespace rimes::capsule;
bool CapsuleService::WantsClipboard() const {
    return Enabled() && clipboard_capture_ && !model_.password_field() &&
           model_.ModuleEnabled(Module::Temporary);
}
void CapsuleService::ConfigureClipboard(bool capture) {
    if (clipboard_capture_ == capture)
        return;
    clipboard_capture_ = capture;
    Publish();
}
void CapsuleService::ReloadHistory(int selected, std::string selected_id) {
    if (reload_cancel_)
        reload_cancel_->store(true);
    reload_cancel_ = std::make_shared<std::atomic<bool>>(false);
    const auto cancelled = reload_cancel_;
    const bool preserve = selected < 0;
    if (preserve)
        selected = history_offset_ + model_.selected();
    const auto query = model_.query(), filter = model_.filter();
    if (preserve && selected_id.empty() && model_.Selected())
        selected_id = model_.Selected()->id;
    model_.LoadModuleCards(Module::Temporary, {});
    history_offset_ = 0;
    history_count_ = 0;
    history_error_.clear();
    if (!AllowsModule()) {
        model_.set_loading(false);
        return;
    }
    model_.set_loading(true);
    const auto root = ClipboardHistoryStore::DefaultRoot();
    if (!storage_jobs_.Run(cancelled, [this, root, cancelled, alive = alive_, post = post_, query,
                                       filter, selected, selected_id] {
            ClipboardPage page;
            bool ok = false;
            try {
                ClipboardHistoryStore store(root);
                page = store.Page(query, filter, selected);
                ok = true;
            } catch (const std::exception&) {
            }
            if (!alive->load() || cancelled->load())
                return;
            post([this, alive, cancelled, ok, selected_id, page = std::move(page)] {
                if (!alive->load() || cancelled->load() || !AllowsModule() ||
                    model_.module() != Module::Temporary)
                    return;
                model_.set_loading(false);
                history_offset_ = page.offset;
                history_count_ = page.count;
                std::vector<Card> cards;
                for (const auto& entry : page.entries) {
                    Card card;
                    card.id = entry.id;
                    card.revision = entry.revision;
                    card.title = entry.title;
                    card.preview = entry.preview;
                    card.module = Module::Temporary;
                    card.filter = entry.filter;
                    card.kind = entry.insertable          ? Kind::Note
                                : entry.filter == "image" ? Kind::Image
                                                          : Kind::Resource;
                    card.source = entry.source.application.empty()
                                      ? (entry.source.backend == "wayland" ? "Wayland" : "X11")
                                      : entry.source.application;
                    card.external_search = true;
                    card.favorite = entry.favorite;
                    cards.push_back(std::move(card));
                }
                model_.LoadModuleCards(Module::Temporary, std::move(cards));
                model_.Select(page.selected - page.offset);
                if (!selected_id.empty()) {
                    Command command;
                    command.id = selected_id;
                    SelectIdentity(command);
                }
                if (!ok)
                    history_error_ = "剪贴板历史无法读取，请检查数据目录权限";
                Publish();
            });
        })) {
        model_.set_loading(false);
        history_error_ = "剪贴板历史正忙，请重新搜索或打开";
    }
}
void CapsuleService::SelectIndex(int index) {
    if (model_.module() != Module::Temporary) {
        model_.Select(index);
        return;
    }
    index = std::clamp(index, 0, std::max(0, history_count_ - 1));
    if (index >= history_offset_ &&
        index < history_offset_ + static_cast<int>(model_.cards().size()))
        model_.Select(index - history_offset_);
    else
        ReloadHistory(index);
}
void CapsuleService::MoveSelection(int delta) {
    SelectIndex(model_.selected() + (model_.module() == Module::Temporary ? history_offset_ : 0) +
                delta);
}
bool CapsuleService::HistoryAction(const std::string& action) {
    if (!AllowsModule() || !model_.visible() || model_.module() != Module::Temporary ||
        model_.loading())
        return false;
    const auto* selected = model_.Selected();
    if (!selected && action != "clear")
        return false;
    const Card card = selected ? *selected : Card{};
    const auto token = model_.token();
    // Publish selection/scope changes before taking an asynchronous lease.
    Publish();
    if (history_action_cancel_)
        history_action_cancel_->store(true);
    history_action_cancel_ = std::make_shared<std::atomic<bool>>(false);
    const auto cancelled = history_action_cancel_, alive = alive_;
    const auto root = ClipboardHistoryStore::DefaultRoot(),
               plugins = rimes::runtime::UserRoot() / "plugins";
    return storage_jobs_.Run(cancelled, [this, alive, cancelled, post = post_, root, plugins, card,
                                         action, token] {
        std::string text;
        bool ok = false, insertable = false;
        try {
            rimes::plugins::PackageStore packages(
                plugins, rimes::plugins::OfficialCatalog(), rimes::plugins::BundledPackages(),
                rimes::plugins::CompiledAdapters(), RIMES_PRODUCT_VERSION);
            const auto authority = packages.Inspect(ModulePlugin(Module::Temporary));
            packages.WithEnabledGrant(authority.id, authority.grant, [&] {
                if (!alive->load() || cancelled->load())
                    return;
                ClipboardHistoryStore store(root);
                if (action == "clear") {
                    store.ClearTemporary();
                    ok = true;
                    return;
                }
                const auto entry = store.Get(card.id);
                if (!entry || entry->revision != card.revision)
                    return;
                if (action == "favorite")
                    ok = store.Favorite(card.id, card.revision, !entry->favorite);
                else if (action == "remove")
                    ok = store.Remove(card.id, card.revision);
                else if (action == "activate" && entry->insertable) {
                    const auto archive = store.Read(card.id, card.revision);
                    if (archive) {
                        auto projection = ProjectClipboard(*archive);
                        text = std::move(projection.text);
                        insertable = projection.insertable;
                        ok = true;
                    }
                } else
                    ok = true;
            });
        } catch (const std::exception&) {
        }
        if (!alive->load() || cancelled->load())
            return;
        post([this, alive, cancelled, card, action, token, ok, insertable, text = std::move(text)] {
            if (!alive->load() || cancelled->load() || !AllowsModule() || !model_.visible() ||
                model_.module() != Module::Temporary)
                return;
            const auto* current = model_.Selected();
            if (action != "clear" &&
                (!current || current->id != card.id || current->revision != card.revision))
                return;
            if (!ok) {
                ReloadHistory(-1, card.id);
                Publish();
                return;
            }
            if (action == "favorite" || action == "remove" || action == "clear") {
                ReloadHistory(-1, action == "favorite" ? card.id : "");
                Publish();
                return;
            }
            if (action == "activate" && insertable && !model_.managing()) {
                auto* target = LiveTarget();
                if (target && model_.arms(token) && !ProtectedInput(target->capabilityFlags())) {
                    target->commitString(text);
                    Close();
                    return;
                }
            }
            // Detached/rich activation restores the original formats. The UI
            // acknowledges the write before closing; no synthetic paste is sent.
            restore_id_ = card.id;
            restore_revision_ = card.revision;
            ++restore_seq_;
            restore_close_ = action == "activate";
            Publish();
        });
    });
}
} // namespace fcitx
