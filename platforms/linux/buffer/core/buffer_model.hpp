#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rimes::buffer {

// Provenance for one staged block. Linux Default mode only mints Rime,
// clipboard, and local (ASCII/direct) origins. Plugin origins stay in the
// snapshot vocabulary so a later Capsule/Mailbox port can reuse the wire
// format without rewriting macOS Swift.
enum class Origin {
    Rime,
    Clipboard,
    Local,
};

enum class InputRoute {
    DirectToHost,
    CaptureToBuffer,
};

enum class MutationReason {
    Ordinary,
    InputRoute,
    BlockRemoval,
    Pause,
    PrivacyDiscard,
    Delivery,
};

struct Block {
    std::string id;
    std::string text;
    Origin origin = Origin::Rime;
    std::int64_t created_at_ms = 0;
};
struct SourceRange {
    std::string block_id, text;
    std::size_t offset = 0;
};

struct CaretRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool valid = false;
};

// Process-local ordered staging store. Visibility, staged content, and the
// input route are independent, matching macOS BufferModel.
class BufferModel {
public:
    static constexpr int kMaxBlocks = 200;
    static constexpr std::size_t kMaxBlockBytes = 1024 * 1024;
    static constexpr int kPreferredLatinWords = 4;
    static constexpr int kProtectedCharacterCount = 160;

    const std::vector<Block>& blocks() const { return blocks_; }
    int insertion_index() const { return insertion_index_; }
    bool all_content_selected() const { return all_content_selected_; }
    bool visible() const { return visible_; }
    bool capture_enabled() const { return capture_enabled_; }
    bool close_after_last() const { return close_after_last_; }
    bool secure() const { return secure_; }
    bool password_field() const { return password_field_; }
    const std::string& capture_token() const { return capture_token_; }
    const std::string& preedit() const { return preedit_; }
    const std::string& target_name() const { return target_name_; }
    const CaretRect& caret() const { return caret_; }
    std::uint64_t generation() const { return generation_; }
    MutationReason last_reason() const { return last_reason_; }
    double hold_progress() const { return hold_progress_; }

    InputRoute input_route() const {
        return capture_enabled_ ? InputRoute::CaptureToBuffer : InputRoute::DirectToHost;
    }

    bool processing_active() const { return capture_enabled_ || transient_enabled_; }
    bool empty() const { return blocks_.empty(); }
    std::string staged_text() const;
    std::size_t pending_count() const { return blocks_.size(); }

    void set_close_after_last(bool value);
    void set_visible(bool value);
    void set_secure(bool value);
    void set_password_field(bool value);
    void set_preedit(std::string_view preedit);
    void set_target_name(std::string_view name);
    void set_caret(const CaretRect& caret);
    void set_hold_progress(double progress);

    void activate_capture(std::string_view token);
    bool captures(std::string_view token) const;
    void route_direct_preserving_content(std::string_view reason);
    void pause_capture_preserving_content();
    void resume_workbench_processing();
    void discard_for_privacy();

    bool append(std::string_view text, Origin origin = Origin::Rime);
    bool append_direct_fragment(std::string_view text, std::string_view owner);
    bool delete_backward_direct(std::string_view owner);
    void finish_direct_run();
    bool insert_pasted_text(std::string_view text, Origin origin = Origin::Clipboard);
    bool remove_last_block();
    bool remove_block(std::string_view id);
    bool set_insertion_point(int index);
    bool select_all();
    void clear_selection();
    void consume_delivered(const std::vector<std::string>& ids);
    bool matches_source(const std::vector<SourceRange>& ranges) const;
    bool consume_source(const std::vector<SourceRange>& ranges);

    const Block* block(std::string_view id) const;
    std::vector<std::string> SplitSegments(std::string_view text) const;

private:
    int ClampedInsertion() const;
    void ClampInsertion();
    void Notify(MutationReason reason = MutationReason::Ordinary);
    void SettleTransientIfIdle();
    bool RemoveSelection();

    std::vector<Block> blocks_;
    int insertion_index_ = 0;
    bool all_content_selected_ = false;
    bool visible_ = false;
    bool capture_enabled_ = false;
    bool transient_enabled_ = false;
    bool close_after_last_ = true;
    bool secure_ = false;
    bool password_field_ = false;
    std::string capture_token_;
    std::string preedit_;
    std::string target_name_;
    CaretRect caret_;
    std::uint64_t generation_ = 0;
    MutationReason last_reason_ = MutationReason::Ordinary;
    double hold_progress_ = 0;
    std::optional<std::string> direct_tail_id_;
    std::string direct_owner_;
};

std::string OriginTag(Origin origin);
bool ParseOrigin(std::string_view tag, Origin* origin);

}  // namespace rimes::buffer
