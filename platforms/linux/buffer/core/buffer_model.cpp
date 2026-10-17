#include "buffer_model.hpp"

#include "buffer_ids.hpp"

#include <algorithm>
#include <cctype>

namespace rimes::buffer {
namespace {

bool IsSentencePunct(char32_t ch) {
    return ch == U'。' || ch == U'！' || ch == U'？' || ch == U'!' || ch == U'?' ||
           ch == U'\n';
}

bool IsLatinLetter(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

int Utf8CharCount(std::string_view text) {
    int count = 0;
    for (unsigned char byte : text) {
        if ((byte & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

std::string Join(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& part : parts) {
        out += part;
    }
    return out;
}

}  // namespace

std::string OriginTag(Origin origin) {
    switch (origin) {
        case Origin::Rime:
            return "rime";
        case Origin::Clipboard:
            return "clipboard";
        case Origin::Local:
            return "local";
    }
    return "rime";
}

bool ParseOrigin(std::string_view tag, Origin* origin) {
    if (origin == nullptr) {
        return false;
    }
    if (tag == "rime") {
        *origin = Origin::Rime;
        return true;
    }
    if (tag == "clipboard") {
        *origin = Origin::Clipboard;
        return true;
    }
    if (tag == "local") {
        *origin = Origin::Local;
        return true;
    }
    return false;
}

std::string BufferModel::staged_text() const {
    return Join([&] {
        std::vector<std::string> texts;
        texts.reserve(blocks_.size());
        for (const auto& block : blocks_) {
            texts.push_back(block.text);
        }
        return texts;
    }());
}

void BufferModel::set_close_after_last(bool value) {
    if (close_after_last_ == value) {
        return;
    }
    close_after_last_ = value;
    Notify();
}

void BufferModel::set_visible(bool value) {
    if (visible_ == value) {
        return;
    }
    visible_ = value;
    Notify();
}

void BufferModel::set_secure(bool value) {
    if (secure_ == value) {
        return;
    }
    secure_ = value;
    Notify();
}

void BufferModel::set_password_field(bool value) {
    if (password_field_ == value) {
        return;
    }
    password_field_ = value;
    Notify();
}

void BufferModel::set_preedit(std::string_view preedit) {
    if (preedit_ == preedit) {
        return;
    }
    preedit_ = std::string(preedit);
    Notify();
}

void BufferModel::set_target_name(std::string_view name) {
    if (target_name_ == name) {
        return;
    }
    target_name_ = std::string(name);
    Notify();
}

void BufferModel::set_caret(const CaretRect& caret) {
    caret_ = caret;
}

void BufferModel::set_hold_progress(double progress) {
    const double clamped = std::clamp(progress, 0.0, 1.0);
    if (hold_progress_ == clamped) {
        return;
    }
    hold_progress_ = clamped;
}

void BufferModel::activate_capture(std::string_view token) {
    const bool changed = !capture_enabled_ || capture_token_ != token;
    capture_enabled_ = true;
    capture_token_ = std::string(token);
    if (!changed) {
        return;
    }
    direct_tail_id_.reset();
    direct_owner_.clear();
    Notify(MutationReason::InputRoute);
}

bool BufferModel::captures(std::string_view token) const {
    if (!capture_enabled_ || token.empty()) {
        return false;
    }
    return capture_token_ == token;
}

void BufferModel::route_direct_preserving_content(std::string_view) {
    if (!capture_enabled_ && capture_token_.empty()) {
        return;
    }
    capture_enabled_ = false;
    capture_token_.clear();
    if (!blocks_.empty()) {
        transient_enabled_ = true;
    }
    preedit_.clear();
    hold_progress_ = 0;
    direct_tail_id_.reset();
    direct_owner_.clear();
    Notify(MutationReason::InputRoute);
}

void BufferModel::pause_capture_preserving_content() {
    capture_enabled_ = false;
    capture_token_.clear();
    transient_enabled_ = false;
    visible_ = false;
    preedit_.clear();
    hold_progress_ = 0;
    all_content_selected_ = false;
    direct_tail_id_.reset();
    direct_owner_.clear();
    Notify(MutationReason::Pause);
}

void BufferModel::resume_workbench_processing() {
    if (processing_active() || blocks_.empty()) {
        return;
    }
    transient_enabled_ = true;
    visible_ = true;
    Notify(MutationReason::InputRoute);
}

void BufferModel::discard_for_privacy() {
    const bool had_state = !blocks_.empty() || capture_enabled_ || visible_ ||
                           !preedit_.empty();
    blocks_.clear();
    insertion_index_ = 0;
    all_content_selected_ = false;
    transient_enabled_ = false;
    capture_enabled_ = false;
    capture_token_.clear();
    preedit_.clear();
    hold_progress_ = 0;
    direct_tail_id_.reset();
    direct_owner_.clear();
    if (had_state) {
        Notify(MutationReason::PrivacyDiscard);
    }
}

bool BufferModel::append(std::string_view text, Origin origin) {
    if (text.empty() || text.size() > kMaxBlockBytes) {
        return false;
    }
    if (static_cast<int>(blocks_.size()) >= kMaxBlocks) {
        return false;
    }
    if (origin == Origin::Rime) {
        RemoveSelection();
    }
    if (!capture_enabled_) {
        transient_enabled_ = true;
    }
    direct_tail_id_.reset();
    direct_owner_.clear();
    const int index = ClampedInsertion();
    Block block;
    block.id = NewBlockId();
    block.text = std::string(text);
    block.origin = origin;
    block.created_at_ms = UnixTimeMs();
    blocks_.insert(blocks_.begin() + index, std::move(block));
    insertion_index_ = index + 1;
    Notify();
    return true;
}

bool BufferModel::append_direct_fragment(std::string_view text, std::string_view owner) {
    if (text.empty() || owner.empty()) {
        return false;
    }
    RemoveSelection();
    std::string prefix;
    int existing_index = -1;
    if (direct_tail_id_ && direct_owner_ == owner) {
        for (int index = 0; index < static_cast<int>(blocks_.size()); ++index) {
            if (blocks_[static_cast<std::size_t>(index)].id == *direct_tail_id_ &&
                index == insertion_index_ - 1 &&
                blocks_[static_cast<std::size_t>(index)].origin == Origin::Local) {
                existing_index = index;
                prefix = blocks_[static_cast<std::size_t>(index)].text;
                break;
            }
        }
    }
    if (existing_index < 0) {
        direct_tail_id_.reset();
        direct_owner_.clear();
    }
    const auto segments = SplitSegments(prefix + std::string(text));
    if (segments.empty()) {
        return false;
    }
    if (existing_index >= 0) {
        blocks_[static_cast<std::size_t>(existing_index)].text = segments.front();
        int insertion = existing_index + 1;
        for (std::size_t index = 1; index < segments.size(); ++index) {
            if (static_cast<int>(blocks_.size()) >= kMaxBlocks) {
                break;
            }
            Block block;
            block.id = NewBlockId();
            block.text = segments[index];
            block.origin = Origin::Local;
            block.created_at_ms = UnixTimeMs();
            direct_tail_id_ = block.id;
            blocks_.insert(blocks_.begin() + insertion, std::move(block));
            ++insertion;
        }
        if (segments.size() == 1) {
            direct_tail_id_ = blocks_[static_cast<std::size_t>(existing_index)].id;
        }
        insertion_index_ = insertion;
    } else {
        int insertion = ClampedInsertion();
        for (const auto& segment : segments) {
            if (static_cast<int>(blocks_.size()) >= kMaxBlocks) {
                break;
            }
            Block block;
            block.id = NewBlockId();
            block.text = segment;
            block.origin = Origin::Local;
            block.created_at_ms = UnixTimeMs();
            direct_tail_id_ = block.id;
            blocks_.insert(blocks_.begin() + insertion, std::move(block));
            ++insertion;
        }
        insertion_index_ = insertion;
    }
    direct_owner_ = std::string(owner);
    if (!capture_enabled_) {
        transient_enabled_ = true;
    }
    Notify();
    return true;
}

bool BufferModel::delete_backward_direct(std::string_view owner) {
    if (RemoveSelection()) {
        return true;
    }
    if (!direct_tail_id_ || direct_owner_ != owner) {
        direct_tail_id_.reset();
        direct_owner_.clear();
        return false;
    }
    for (int index = 0; index < static_cast<int>(blocks_.size()); ++index) {
        auto& block = blocks_[static_cast<std::size_t>(index)];
        if (block.id != *direct_tail_id_ || index != insertion_index_ - 1) {
            continue;
        }
        if (block.text.size() > 1) {
            // Delete one UTF-8 code point from the tail.
            std::size_t end = block.text.size();
            do {
                --end;
            } while (end > 0 && (static_cast<unsigned char>(block.text[end]) & 0xC0) == 0x80);
            block.text.resize(end);
            Notify(MutationReason::BlockRemoval);
            return true;
        }
        blocks_.erase(blocks_.begin() + index);
        insertion_index_ = index;
        direct_tail_id_.reset();
        direct_owner_.clear();
        SettleTransientIfIdle();
        Notify(MutationReason::BlockRemoval);
        return true;
    }
    direct_tail_id_.reset();
    direct_owner_.clear();
    return false;
}

void BufferModel::finish_direct_run() {
    direct_tail_id_.reset();
    direct_owner_.clear();
}

bool BufferModel::insert_pasted_text(std::string_view text, Origin origin) {
    if (text.empty() || text.size() > kMaxBlockBytes) {
        return false;
    }
    if (text.find('\0') != std::string_view::npos) {
        return false;
    }
    auto segments = SplitSegments(text);
    if (segments.empty()) {
        return false;
    }
    RemoveSelection();
    if (!capture_enabled_) {
        transient_enabled_ = true;
    }
    direct_tail_id_.reset();
    direct_owner_.clear();
    int insertion = ClampedInsertion();
    for (const auto& segment : segments) {
        if (static_cast<int>(blocks_.size()) >= kMaxBlocks) {
            break;
        }
        Block block;
        block.id = NewBlockId();
        block.text = segment;
        block.origin = origin;
        block.created_at_ms = UnixTimeMs();
        blocks_.insert(blocks_.begin() + insertion, std::move(block));
        ++insertion;
    }
    insertion_index_ = insertion;
    Notify();
    return true;
}

bool BufferModel::remove_last_block() {
    if (RemoveSelection()) {
        return true;
    }
    if (blocks_.empty()) {
        return false;
    }
    const int index = static_cast<int>(blocks_.size()) - 1;
    if (direct_tail_id_ && blocks_[static_cast<std::size_t>(index)].id == *direct_tail_id_) {
        direct_tail_id_.reset();
        direct_owner_.clear();
    }
    blocks_.pop_back();
    ClampInsertion();
    SettleTransientIfIdle();
    Notify(MutationReason::BlockRemoval);
    return true;
}

bool BufferModel::remove_block(std::string_view id) {
    for (int index = 0; index < static_cast<int>(blocks_.size()); ++index) {
        if (blocks_[static_cast<std::size_t>(index)].id != id) {
            continue;
        }
        if (direct_tail_id_ && *direct_tail_id_ == id) {
            direct_tail_id_.reset();
            direct_owner_.clear();
        }
        blocks_.erase(blocks_.begin() + index);
        if (insertion_index_ > index) {
            --insertion_index_;
        }
        ClampInsertion();
        SettleTransientIfIdle();
        Notify(MutationReason::BlockRemoval);
        return true;
    }
    return false;
}

bool BufferModel::set_insertion_point(int index) {
    const int target = std::clamp(index, 0, static_cast<int>(blocks_.size()));
    const bool changed = insertion_index_ != target || all_content_selected_;
    insertion_index_ = target;
    all_content_selected_ = false;
    direct_tail_id_.reset();
    direct_owner_.clear();
    if (changed) {
        Notify();
    }
    return true;
}

bool BufferModel::select_all() {
    const bool selected = !blocks_.empty();
    if (all_content_selected_ == selected) {
        return selected;
    }
    all_content_selected_ = selected;
    direct_tail_id_.reset();
    direct_owner_.clear();
    Notify();
    return selected;
}

void BufferModel::clear_selection() {
    if (!all_content_selected_) {
        return;
    }
    all_content_selected_ = false;
    Notify();
}

bool BufferModel::matches_source(const std::vector<SourceRange>& ranges) const {
    if (ranges.empty()) return false;
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        const auto& range = ranges[index];
        const auto* source = block(range.block_id);
        if (!source || range.text.empty() || range.offset > source->text.size() || range.text.size() > source->text.size() - range.offset ||
            source->text.compare(range.offset, range.text.size(), range.text) != 0) return false;
        const auto boundary = [&](std::size_t offset) { return offset == source->text.size() || (static_cast<unsigned char>(source->text[offset]) & 0xC0) != 0x80; };
        if (!boundary(range.offset) || !boundary(range.offset + range.text.size())) return false;
        for (std::size_t other = 0; other < index; ++other) {
            const auto& previous = ranges[other];
            if (previous.block_id == range.block_id && range.offset < previous.offset + previous.text.size() && previous.offset < range.offset + range.text.size()) return false;
        }
    }
    return true;
}
bool BufferModel::consume_source(const std::vector<SourceRange>& ranges) {
    if (!matches_source(ranges)) return false;
    auto ordered = ranges;
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.offset > b.offset; });
    for (const auto& range : ordered)
        for (auto& source : blocks_) if (source.id == range.block_id) { source.text.erase(range.offset, range.text.size()); break; }
    finish_direct_run();
    std::vector<std::string> empty;
    for (const auto& source : blocks_) if (source.text.empty()) empty.push_back(source.id);
    if (empty.empty()) Notify(MutationReason::Delivery);
    else consume_delivered(empty);
    return true;
}

void BufferModel::consume_delivered(const std::vector<std::string>& ids) {
    if (ids.empty()) {
        return;
    }
    int removed_before = 0;
    auto is_delivered = [&](const Block& block) {
        return std::find(ids.begin(), ids.end(), block.id) != ids.end();
    };
    for (int index = 0; index < static_cast<int>(blocks_.size()); ++index) {
        if (is_delivered(blocks_[static_cast<std::size_t>(index)]) && index < insertion_index_) {
            ++removed_before;
        }
    }
    blocks_.erase(std::remove_if(blocks_.begin(), blocks_.end(), is_delivered), blocks_.end());
    if (direct_tail_id_) {
        bool tail_alive = false;
        for (const auto& block : blocks_) {
            if (block.id == *direct_tail_id_) {
                tail_alive = true;
                break;
            }
        }
        if (!tail_alive) {
            direct_tail_id_.reset();
            direct_owner_.clear();
        }
    }
    insertion_index_ -= removed_before;
    ClampInsertion();
    SettleTransientIfIdle();
    Notify(MutationReason::Delivery);
}

const Block* BufferModel::block(std::string_view id) const {
    for (const auto& item : blocks_) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

std::vector<std::string> BufferModel::SplitSegments(std::string_view text) const {
    std::vector<std::string> result;
    if (text.empty()) {
        return result;
    }
    std::string current;
    int latin_words = 0;
    bool inside_latin = false;
    int chars = 0;
    auto flush = [&]() {
        if (current.empty()) {
            return;
        }
        result.push_back(current);
        current.clear();
        latin_words = 0;
        inside_latin = false;
        chars = 0;
    };
    std::size_t offset = 0;
    while (offset < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[offset]);
        std::size_t width = 1;
        if ((lead & 0x80) == 0) {
            width = 1;
        } else if ((lead & 0xE0) == 0xC0) {
            width = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            width = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            width = 4;
        }
        if (offset + width > text.size()) {
            width = 1;
        }
        const std::string_view rune = text.substr(offset, width);
        char32_t code = lead;
        if (width == 3) {
            code = static_cast<char32_t>(((lead & 0x0F) << 12) |
                                         ((static_cast<unsigned char>(text[offset + 1]) & 0x3F) << 6) |
                                         (static_cast<unsigned char>(text[offset + 2]) & 0x3F));
        } else if (width == 2) {
            code = static_cast<char32_t>(((lead & 0x1F) << 6) |
                                         (static_cast<unsigned char>(text[offset + 1]) & 0x3F));
        } else if (width == 4) {
            code = static_cast<char32_t>(
                ((lead & 0x07) << 18) |
                ((static_cast<unsigned char>(text[offset + 1]) & 0x3F) << 12) |
                ((static_cast<unsigned char>(text[offset + 2]) & 0x3F) << 6) |
                (static_cast<unsigned char>(text[offset + 3]) & 0x3F));
        }
        current.append(rune);
        ++chars;
        if (width == 1 && IsLatinLetter(lead)) {
            if (!inside_latin) {
                inside_latin = true;
                ++latin_words;
            }
        } else if (width == 1 && std::isspace(lead) != 0) {
            inside_latin = false;
        } else {
            inside_latin = false;
        }
        const bool sentence = IsSentencePunct(code);
        const bool latin_full = latin_words >= kPreferredLatinWords && width == 1 &&
                                std::isspace(lead) != 0;
        const bool too_long = chars >= kProtectedCharacterCount;
        if (sentence || latin_full || too_long) {
            flush();
        }
        offset += width;
    }
    flush();
    if (result.empty() && Utf8CharCount(text) > 0) {
        result.emplace_back(text);
    }
    return result;
}

int BufferModel::ClampedInsertion() const {
    return std::clamp(insertion_index_, 0, static_cast<int>(blocks_.size()));
}

void BufferModel::ClampInsertion() {
    insertion_index_ = ClampedInsertion();
}

void BufferModel::Notify(MutationReason reason) {
    last_reason_ = reason;
    ++generation_;
}

void BufferModel::SettleTransientIfIdle() {
    if (blocks_.empty() && !capture_enabled_) {
        transient_enabled_ = false;
    }
}

bool BufferModel::RemoveSelection() {
    if (!all_content_selected_ || blocks_.empty()) {
        all_content_selected_ = false;
        return false;
    }
    blocks_.clear();
    insertion_index_ = 0;
    all_content_selected_ = false;
    direct_tail_id_.reset();
    direct_owner_.clear();
    SettleTransientIfIdle();
    Notify(MutationReason::BlockRemoval);
    return true;
}

}  // namespace rimes::buffer
