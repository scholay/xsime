#include "session/protected_input.hpp"
#include "buffer_service.hpp"
#include "buffer_ids.hpp"
#include <algorithm>
namespace fcitx {
namespace {
std::vector<rimes::buffer::SourceRange> Ranges(const rimes::translation::Delivery& delivery) {
    std::vector<rimes::buffer::SourceRange> result;
    for (const auto& slice : delivery.source) result.push_back({slice.block, slice.text, slice.offset});
    return result;
}
std::vector<rimes::buffer::SourceRange> Ranges(const rimes::buffer::DerivedRequest& request) {
    std::vector<rimes::buffer::SourceRange> result;
    for (const auto& block : request.source) result.push_back({block.id, block.text, 0});
    return result;
}
}
void BufferService::SynchronizeResultDelivery() {
    const auto* result = derived_.SelectedResult();
    if (!derived_request_ || derived_.Phase() != rimes::buffer::DerivedPhase::Ready || !result) {
        result_pending_.clear(); result_pending_id_.clear(); return;
    }
    if (result_pending_id_ == result->id) return;
    result_pending_.clear();
    std::vector<std::string> segments;
    const auto logical = derived_logical_blocks_.find(result->alternative);
    if (logical != derived_logical_blocks_.end()) {
        for (const auto& block : logical->second) {
            const auto children = model_.SplitSegments(block);
            segments.insert(segments.end(), children.begin(), children.end());
        }
    } else segments = model_.SplitSegments(result->text);
    if (segments.size() > 200) {
        for (std::size_t i = 200; i < segments.size(); ++i) segments[199] += segments[i];
        segments.resize(200);
    }
    std::string joined;
    for (const auto& text : segments) joined += text;
    if (joined != result->text) return;
    for (auto& text : segments) result_pending_.push_back({rimes::buffer::NewBlockId(), std::move(text), rimes::buffer::Origin::Local, 0});
    result_pending_id_ = result->id;
}
bool BufferService::CanDeliverDerived() {
    auto* target = LiveTarget();
    if (!target || !model_.visible() || !model_.capture_enabled() || model_.secure() || model_.password_field() ||
        ProtectedInput(target->capabilityFlags()) || !model_.preedit().empty() || delivery_conflict_) return false;
    try {
        if (TranslationActive()) {
            const auto lease = *translation_lease_;
            if (!validate_ || !validate_(lease) || !TranslationActive()) return false;
            const auto head = translation_.Head();
            return head && (head->retired || model_.matches_source(Ranges(*head))) && LiveTarget() == target;
        }
        const auto request = derived_request_;
        const bool grant = request && validate_ && validate_(request->lease);
        return request && derived_.Owns(*request, grant) && derived_.Phase() == rimes::buffer::DerivedPhase::Ready &&
            !result_pending_.empty() && model_.matches_source(Ranges(*request)) && LiveTarget() == target;
    } catch (const std::exception&) { return false; }
}
bool BufferService::DeliverDerived(bool all) {
    SynchronizeDerived();
    if (!CanDeliverDerived()) return false;
    auto* const target = LiveTarget();
    const auto epoch = focus_epoch_;
    const bool translating = TranslationActive();
    const auto scope = translating ? translation_scope_ : derived_request_->id;
    bool sent = false;
    for (unsigned count = 0; count < 200; ++count) {
        SynchronizeDerived();
        if (!CanDeliverDerived() || LiveTarget() != target || focus_epoch_ != epoch) break;
        if (translating) {
            // Head() and CanDeliverDerived() stop at the first unfinished unit.
            // A running tail must not prevent delivery of the ready prefix.
            if (!TranslationActive() || translation_scope_ != scope) break;
            const auto head = translation_.Head().value();
            try { target->commitString(head.text); }
            catch (const std::exception&) { delivery_conflict_ = true; break; }
            sent = true;
            // A re-entrant frontend may change the source during native commit.
            // Preserve changed text and stop repeats rather than deleting it.
            const bool retired = head.retired || model_.consume_source(Ranges(head));
            const bool consumed = translation_.Consume(head);
            if (!retired || !consumed) { delivery_conflict_ = true; CancelTranslation(false); }
            translation_result_id_ = rimes::buffer::NewBlockId();
        } else {
            const auto request = derived_request_;
            if (!request || request->id != scope || result_pending_.empty()) break;
            const auto child = result_pending_.front();
            const auto result_id = result_pending_id_;
            try { target->commitString(child.text); }
            catch (const std::exception&) { delivery_conflict_ = true; break; }
            sent = true;
            const bool consumed = derived_.ConsumePrefix(*request, true, result_id, child.text);
            if (!consumed) { delivery_conflict_ = true; CancelDerived(); break; }
            result_pending_.erase(result_pending_.begin());
            result_pending_id_ = derived_.SelectedResult()->id;
            if (result_pending_.empty()) {
                if (!model_.consume_source(Ranges(*request))) delivery_conflict_ = true;
                CancelDerived();
            }
        }
        if (!all || delivery_conflict_) break;
    }
    if (delivery_conflict_) Publish();
    else if (sent) {
        const bool drained = translating ? translation_.All().empty() && translation_.Retired().empty() : result_pending_.empty();
        if (drained && model_.empty() && model_.close_after_last()) CloseAndPause();
        else Publish();
    }
    return sent;
}
} // namespace fcitx
