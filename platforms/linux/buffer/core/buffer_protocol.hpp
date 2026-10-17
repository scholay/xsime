#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "buffer_model.hpp"
#include "derived_workspace.hpp"

namespace rimes::buffer {

enum class CommandOp {
    Hello,
    Query,
    Toggle,
    Show,
    Close,
    SendNext,
    SendAll,
    RemoveLast,
    SelectAll,
    Paste,
    SetInsertion,
    DragBegin,
    DragEnd,
    SelectPlugin,
    Generate,
    CancelGeneration,
    ReturnToEdit,
    SelectResult,
    CopyResult,
    CycleAutoSend,
    ToggleCloseAfterLast,
    Unknown,
};

struct Command {
    CommandOp op = CommandOp::Unknown;
    std::string text;
    int insertion_index = -1;
    std::string id;
};

struct PluginChoice { std::string id, name; };
struct Snapshot {
    int width = 760;
    std::uint64_t generation = 0;
    bool visible = false;
    bool capturing = false;
    bool enabled = false;
    bool secure = false;
    bool password_field = false;
    bool close_after_last = true;
    bool empty = true;
    int insertion_index = 0;
    double hold_progress = 0;
    int auto_send_seconds = 0;
    std::map<std::string, double> auto_send_progress;
    std::string preedit;
    std::string placeholder;
    std::string target;
    std::string staged_text;
    CaretRect caret;
    std::vector<Block> blocks;
    int ui_clients = 0;
    std::string theme = "night";
    std::string active_plugin;
    bool live_expand = true;
    std::vector<PluginChoice> plugins;
    DerivedPhase phase = DerivedPhase::Idle;
    std::string message;
    bool can_generate = false;
    bool can_deliver = false;
    std::string selected_result;
    std::vector<DerivedResult> results;
    std::vector<Block> result_blocks;
};

Snapshot MakeSnapshot(const BufferModel& model);

std::string EncodeSnapshot(const Snapshot& snapshot);
std::string EncodeError(std::string_view code, std::string_view message);
bool ParseCommand(std::string_view json, Command* command, std::string* error);
bool ParseSnapshot(std::string_view json, Snapshot* snapshot);

std::string JsonEscape(std::string_view text);
bool JsonUnescape(std::string_view text, std::string* output);

// 4-byte big-endian length + UTF-8 payload. Rejects frames above 1 MiB.
bool EncodeFrame(std::string_view payload, std::string* frame);
bool DecodeFrameHeader(const char header[4], std::uint32_t* length);

inline constexpr std::uint32_t kMaxFrameBytes = 1024 * 1024;
inline constexpr const char* kPlaceholder = "Type to stage, then Return to send";
inline constexpr const char* kProtocolVersion = "1";

}  // namespace rimes::buffer
