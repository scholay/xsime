#include "buffer_service.hpp"
#include "buffer_ids.hpp"
#include <fcitx-utils/event.h>

namespace fcitx {
namespace {
constexpr const char* kTranslation = "builtin.apple-translation";
std::int64_t MonotonicMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}
bool BufferService::Translating() const {
    return plugins_ && plugins_->SelectedBuffer() == kTranslation && derived_adapters_.count(kTranslation);
}
bool BufferService::TranslationActive() const {
    return Translating() && translation_lease_ && translation_configuration_ && !translation_scope_.empty() &&
        model_.visible() && model_.processing_active() && !focus_suspended_ && !model_.secure() && !model_.password_field();
}
void BufferService::CancelTranslation(bool clear) {
    if (translation_cancel_) translation_cancel_->store(true);
    translation_cancel_.reset();
    translation_timer_.reset();
    translation_unit_.clear(); translation_ticket_.clear();
    translation_explicit_ = false;
    translation_.Pause();
    if (clear) {
        translation_.ResetMutable(); translation_scope_.clear(); translation_result_id_.clear();
        translation_source_invalid_ = false;
        translation_lease_.reset(); translation_configuration_.reset();
    }
}
void BufferService::SynchronizeTranslation() {
    if (!Translating() || !model_.visible() || !model_.processing_active() || focus_suspended_ || model_.secure() || model_.password_field()) { CancelTranslation(true); return; }
    try {
        auto lease = plugins_->Authorize(kTranslation, "translation.generate");
        if (!lease || !validate_ || !validate_(*lease) || !freeze_) { CancelTranslation(true); return; }
        const auto scope = configuration_revision_ + ":" + lease->registry + ":" + std::to_string(lease->generation) + ":" + model_.capture_token() + ":" + std::to_string(focus_epoch_);
        if (scope != translation_scope_) {
            CancelTranslation(true);
            auto config = freeze_(kTranslation);
            if (config.revision != configuration_revision_) return;
            translation_configuration_ = std::move(config);
            translation_lease_ = std::move(lease);
            translation_scope_ = scope;
        }
        if (!translation_configuration_) return;
        std::vector<std::string> old;
        for (const auto& unit : translation_.All()) old.push_back(unit.id);
        std::vector<rimes::translation::SourceBlock> source;
        for (const auto& block : model_.blocks()) source.push_back({block.id, block.text});
        translation_source_invalid_ = !translation_.Synchronize(source, MonotonicMs());
        std::vector<std::string> current;
        for (const auto& unit : translation_.All()) current.push_back(unit.id);
        if (old != current) { translation_result_id_ = rimes::buffer::NewBlockId(); translation_explicit_ = false; }
        if (translation_result_id_.empty() && !translation_.Retired().empty()) translation_result_id_ = rimes::buffer::NewBlockId();
        if (!translation_unit_.empty() && !translation_.Find(translation_unit_)) {
            if (translation_cancel_) translation_cancel_->store(true);
            translation_cancel_.reset(); translation_unit_.clear(); translation_ticket_.clear();
        }
        const bool automatic = translation_configuration_->plugin.value("continuous", true) || translation_explicit_;
        if (instance_ && automatic && translation_.HasPending() && !translation_.Running() && !translation_.Paused() && !translation_timer_) {
            translation_timer_ = instance_->eventLoop().addTimeEvent(CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 50000, 0,
                [this](EventSourceTime*, std::uint64_t) { translation_timer_.reset(); TranslationTick(); return true; });
        }
    } catch (const std::exception&) { CancelTranslation(true); }
}
bool BufferService::TranslationTick(bool force) {
    SynchronizeDerived();
    if (!TranslationActive() || !translation_configuration_ || !translation_lease_) return false;
    // A scheduled debounce and an explicit Generate can meet in one event-loop
    // turn. Never replace the sole cancellation handle of an in-flight unit.
    if (translation_.Running()) return false;
    if (force) { translation_.Retry(); translation_explicit_ = true; }
    if (!force && !translation_explicit_ && !translation_configuration_->plugin.value("continuous", true)) return false;
    const auto unit = translation_.Begin(MonotonicMs(), force);
    if (!unit) { Publish(); return false; }
    translation_timer_.reset();
    if (translation_cancel_) translation_cancel_->store(true);
    translation_unit_ = unit->id; translation_ticket_ = unit->ticket;
    translation_cancel_ = std::make_shared<std::atomic<bool>>(false);
    const auto config = *translation_configuration_;
    const auto lease = *translation_lease_;
    const auto scope = translation_scope_;
    rimes::buffer::DerivedRequest request;
    request.id = unit->ticket; request.lifetime = scope; request.configuration = config.revision;
    request.lease = lease; request.focus = {model_.capture_token(), focus_epoch_};
    request.source = model_.blocks(); request.source_revision = model_.generation(); request.text = unit->source;
    auto receive = [this, alive = alive_, post = post_, scope, lease, id = unit->id, ticket = unit->ticket,
                    target = config.plugin.value("targetLanguage", std::string("en"))](ResultUpdate update) {
        const auto alternative = update.alternative;
        const auto finished = update.finished, success = update.success;
        auto text = std::move(update.text);
        if (!finished || alternative || !alive->load()) return;
        post([this, alive, scope, lease, id, ticket, target, text = std::move(text), success] {
            if (!alive->load()) return;
            SynchronizeDerived();
            bool grant = false;
            try { grant = validate_ && validate_(lease); } catch (const std::exception&) {}
            if (!grant || scope != translation_scope_ || id != translation_unit_ || ticket != translation_ticket_) {
                // Synchronization may already have cleared the old plaintext.
                // Publish that revocation even though this reply is rejected.
                Publish(); return;
            }
            translation_.Finish(id, ticket, text, success, target);
            translation_unit_.clear(); translation_ticket_.clear(); translation_cancel_.reset();
            Publish();
        });
    };
    try { derived_adapters_.at(kTranslation).run(std::move(request), config, translation_cancel_, std::move(receive)); }
    catch (const std::exception&) {
        translation_.Finish(unit->id, unit->ticket, "", false, config.plugin.value("targetLanguage", std::string("en")));
        if (translation_cancel_) translation_cancel_->store(true);
        translation_cancel_.reset(); translation_unit_.clear(); translation_ticket_.clear();
    }
    Publish();
    return true;
}
} // namespace fcitx
