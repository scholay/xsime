#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "capsule_model.hpp"

namespace rimes::capsule {

enum class CommandOp {
    Hello,
    Status,
    Toggle,
    Show,
    Close,
    Next,
    Prev,
    Select,
    Tab,
    Activate,
    Copy,
    Search,
    Module,
    Filter,
    Manage,
    Rail,
    Refresh,
    Detail,
    DetailClose,
    RowSelect,
    RowCopy,
    RowInsert,
    Favorite,
    Remove,
    ClearHistory,
    Unknown,
};

struct Command {
    CommandOp op = CommandOp::Unknown;
    std::string text;
    int index = -1;
    std::string id;
    std::string revision;
};

struct Snapshot {
    std::uint64_t generation = 0;
    bool visible = false;
    bool armed = false;
    bool password_field = false;
    std::string theme = "night";
    std::string module = "notes";
    std::string filter = "all";
    bool managing = false;
    bool loading = false;
    bool clipboard_capture = false;
    std::string restore_id, restore_revision;
    std::uint64_t restore_seq = 0;
    bool restore_close = false;
    std::vector<Module> modules{Module::Notes};
    std::string tab;
    std::string query;
    std::string hint;
    std::string last_copied;
    std::uint64_t copy_seq = 0;
    std::string selected_id;
    std::string selected_title;
    std::string selected_preview;
    int selected = 0;
    int offset = 0;
    int count = 0;
    int ui_clients = 0;
    std::vector<Card> cards;
    bool detail = false;
    std::string detail_revision;
    std::string detail_notice;
    int detail_selected = -1, detail_offset = 0, detail_count = 0;
    std::vector<DetailRow> detail_rows;
    std::vector<std::string> detail_headings;
};

Snapshot MakeSnapshot(const CapsuleModel& model);
std::string EncodeSnapshot(const Snapshot& snapshot);
bool DecodeSnapshot(std::string_view json, Snapshot* snapshot);
bool ParseCommand(std::string_view json, Command* command, std::string* error);

inline constexpr const char* kProtocolVersion = "1";

}  // namespace rimes::capsule
