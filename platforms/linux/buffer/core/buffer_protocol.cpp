#include "native_buffer_metrics.hpp"
#include "buffer_protocol.hpp"
#include "plugin_store.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rimes::buffer {
namespace {
using Json = plugins::Json;
Json Parse(std::string_view text) {
    if (text.size() > kMaxFrameBytes) throw std::runtime_error("Frame too large");
    auto value = Json::parse(text, [](int depth, Json::parse_event_t, Json&) {
        if (depth > 32) throw std::runtime_error("JSON nesting too deep");
        return true;
    });
    if (!value.is_object() || (value.contains("v") && value.at("v") != 1))
        throw std::runtime_error("Invalid protocol object");
    return value;
}
const char* PhaseName(DerivedPhase phase) {
    switch (phase) {
        case DerivedPhase::Idle: return "idle";
        case DerivedPhase::Running: return "running";
        case DerivedPhase::Ready: return "ready";
        case DerivedPhase::Failed: return "failed";
        case DerivedPhase::Protected: return "protected";
    }
    return "protected";
}
DerivedPhase ReadPhase(const std::string& phase) {
    for (auto item : {DerivedPhase::Idle, DerivedPhase::Running, DerivedPhase::Ready,
                      DerivedPhase::Failed, DerivedPhase::Protected})
        if (phase == PhaseName(item)) return item;
    throw std::runtime_error("Unknown result phase");
}
std::string ReadText(const Json& value, const char* key, const std::string& fallback = "") {
    auto text = value.value(key, fallback);
    if (text.find('\0') != std::string::npos) throw std::runtime_error("NUL in text");
    return text;
}
} // namespace

Snapshot MakeSnapshot(const BufferModel& model) {
    Snapshot snapshot;
    snapshot.generation = model.generation();
    snapshot.visible = model.visible();
    snapshot.capturing = model.capture_enabled();
    snapshot.enabled = model.processing_active();
    snapshot.secure = model.secure() || model.password_field();
    snapshot.password_field = model.password_field();
    snapshot.close_after_last = model.close_after_last();
    snapshot.empty = model.empty();
    snapshot.insertion_index = model.insertion_index();
    snapshot.hold_progress = model.hold_progress();
    snapshot.preedit = model.preedit();
    snapshot.placeholder = kPlaceholder;
    snapshot.target = model.target_name();
    snapshot.caret = model.caret();
    if (snapshot.secure) {
        snapshot.target.clear();
        snapshot.caret = {};
        snapshot.preedit.clear();
        snapshot.staged_text.clear();
        snapshot.blocks.clear();
        snapshot.empty = true;
        return snapshot;
    }
    snapshot.staged_text = model.staged_text();
    snapshot.blocks = model.blocks();
    return snapshot;
}

std::string EncodeSnapshot(const Snapshot& snapshot) {
    if (snapshot.password_field && !snapshot.secure) {
        auto protected_snapshot = snapshot; protected_snapshot.secure = true;
        return EncodeSnapshot(protected_snapshot);
    }
    const auto caret = snapshot.secure ? CaretRect{} : snapshot.caret;
    Json blocks = Json::array(), results = Json::array(), choices = Json::array(), segments = Json::array();
    if (!snapshot.secure) {
        for (const auto& b : snapshot.blocks)
            blocks.push_back({{"id", b.id}, {"text", b.text}, {"origin", OriginTag(b.origin)},
                              {"created_at_ms", b.created_at_ms},
                              {"auto_send_progress", snapshot.auto_send_progress.count(b.id) ? snapshot.auto_send_progress.at(b.id) : 0.0}});
        for (const auto& r : snapshot.results)
            results.push_back({{"id", r.id}, {"text", r.text}, {"alternative", r.alternative}, {"ready", r.ready}});
        // Sizes refer to the selected result, without duplicating its text.
        for (const auto& block : snapshot.result_blocks)
            segments.push_back({{"id", block.id}, {"bytes", block.text.size()}});
    }
    for (const auto& p : snapshot.plugins) choices.push_back({{"id", p.id}, {"name", p.name}});
    return Json{{"v", 1}, {"type", "snapshot"}, {"generation", snapshot.generation},
        {"width", snapshot.width}, {"visible", snapshot.visible}, {"capturing", snapshot.capturing},
        {"enabled", snapshot.enabled}, {"secure", snapshot.secure}, {"password_field", snapshot.password_field},
        {"close_after_last", snapshot.close_after_last}, {"empty", snapshot.secure || snapshot.empty},
        {"insertion_index", snapshot.insertion_index}, {"hold_progress", snapshot.hold_progress},
        {"auto_send_seconds", snapshot.auto_send_seconds},
        {"preedit", snapshot.secure ? "" : snapshot.preedit}, {"placeholder", snapshot.placeholder},
        {"target", snapshot.secure ? "" : snapshot.target}, {"staged_text", snapshot.secure ? "" : snapshot.staged_text},
        {"caret", {{"x", caret.x}, {"y", caret.y}, {"w", caret.width},
                   {"h", caret.height}, {"valid", caret.valid}}},
        {"ui_clients", snapshot.ui_clients}, {"blocks", blocks}, {"theme", snapshot.theme},
        {"active_plugin", snapshot.active_plugin}, {"plugins", choices}, {"live_expand", snapshot.live_expand},
        {"phase", PhaseName(snapshot.secure ? DerivedPhase::Protected : snapshot.phase)},
        {"message", snapshot.secure ? "" : snapshot.message},
        {"can_generate", !snapshot.secure && snapshot.can_generate},
        {"can_deliver", !snapshot.secure && snapshot.can_deliver},
        {"selected_result", snapshot.secure ? "" : snapshot.selected_result}, {"results", results}, {"result_blocks", segments}}.dump();
}

bool ParseSnapshot(std::string_view text, Snapshot* output) {
    if (!output) return false;
    try {
        const auto value = Parse(text);
        if (value.at("type") != "snapshot") return false;
        Snapshot s;
        s.width = std::clamp(value.value("width", 760), rimes::native::bufferMinimumWidth, rimes::native::bufferMaximumWidth);
        s.generation = value.value("generation", std::uint64_t{0});
        s.visible = value.at("visible").get<bool>();
        s.capturing = value.at("capturing").get<bool>();
        s.enabled = value.value("enabled", false);
        s.secure = value.at("secure").get<bool>();
        s.password_field = value.value("password_field", false);
        s.secure = s.secure || s.password_field;
        s.close_after_last = value.value("close_after_last", true);
        s.empty = value.at("empty").get<bool>();
        s.insertion_index = value.value("insertion_index", 0);
        s.hold_progress = value.value("hold_progress", 0.0);
        s.auto_send_seconds = value.value("auto_send_seconds", 0);
        if (s.auto_send_seconds != 0 && s.auto_send_seconds != 1 && s.auto_send_seconds != 2 &&
            s.auto_send_seconds != 3 && s.auto_send_seconds != 5) return false;
        if (!std::isfinite(s.hold_progress)) return false;
        s.hold_progress = std::clamp(s.hold_progress, 0.0, 1.0);
        s.preedit = ReadText(value, "preedit");
        s.placeholder = ReadText(value, "placeholder", kPlaceholder);
        s.target = ReadText(value, "target");
        s.staged_text = ReadText(value, "staged_text");
        const auto& c = value.at("caret");
        s.caret = {c.at("x"), c.at("y"), c.at("w"), c.at("h"), c.at("valid")};
        s.ui_clients = value.value("ui_clients", 0);
        if (!value.at("blocks").is_array()) return false;
        for (const auto& b : value.at("blocks")) {
            Block block;
            block.id = ReadText(b, "id");
            block.text = ReadText(b, "text");
            if (block.id.empty() || !ParseOrigin(ReadText(b, "origin"), &block.origin)) return false;
            block.created_at_ms = b.value("created_at_ms", std::int64_t{0});
            const double progress = b.value("auto_send_progress", 0.0);
            if (!std::isfinite(progress) || progress < 0 || progress > 1) return false;
            s.auto_send_progress[block.id] = progress;
            s.blocks.push_back(std::move(block));
        }
        s.theme = ReadText(value, "theme", "night");
        if (s.theme != "night" && s.theme != "day" && s.theme != "quiet" && s.theme != "rasta" && s.theme != "liquidGlass") return false;
        s.active_plugin = ReadText(value, "active_plugin");
        s.live_expand = value.value("live_expand", true);
        if (value.contains("plugins")) {
            if (!value.at("plugins").is_array() || value.at("plugins").size() > 100) return false;
            for (const auto& p : value.at("plugins")) s.plugins.push_back({ReadText(p, "id"), ReadText(p, "name")});
        }
        s.phase = ReadPhase(ReadText(value, "phase", "idle"));
        s.message = ReadText(value, "message", "");
        if (s.message.size() > 512) return false;
        s.can_generate = value.value("can_generate", false);
        s.can_deliver = value.value("can_deliver", false);
        s.selected_result = ReadText(value, "selected_result");
        if (value.contains("results")) {
            if (!value.at("results").is_array() || value.at("results").size() > 5) return false;
            for (const auto& r : value.at("results")) {
                DerivedResult result{ReadText(r, "id"), ReadText(r, "text"), r.at("alternative"), r.at("ready")};
                if (result.id.empty() || result.alternative >= 5 || (result.ready && s.phase != DerivedPhase::Ready)) return false;
                s.results.push_back(std::move(result));
            }
        }
        if (value.contains("result_blocks")) {
            const auto& parts = value.at("result_blocks");
            if (!parts.is_array() || parts.size() > 200) return false;
            const auto selected = std::find_if(s.results.begin(), s.results.end(), [&](const auto& r) { return r.id == s.selected_result; });
            if (!parts.empty() && selected == s.results.end()) return false;
            std::size_t offset = 0;
            for (const auto& part : parts) {
                const auto id = ReadText(part, "id");
                if (id.empty() || !part.at("bytes").is_number_unsigned()) return false;
                const auto length = part.at("bytes").get<std::size_t>();
                if (!length || length > selected->text.size() - offset) return false;
                auto segment = selected->text.substr(offset, length);
                (void)Json(segment).dump();
                s.result_blocks.push_back({id, std::move(segment), Origin::Local, 0});
                offset += length;
            }
            if (!parts.empty() && offset != selected->text.size()) return false;
        }
        if (s.secure) {
            s.target.clear(); s.caret = {}; s.can_deliver = false;
            s.blocks.clear(); s.results.clear(); s.preedit.clear(); s.staged_text.clear();
            s.result_blocks.clear();
            s.auto_send_progress.clear();
            s.selected_result.clear(); s.message.clear(); s.empty = true; s.can_generate = false; s.phase = DerivedPhase::Protected;
        }
        *output = std::move(s);
        return true;
    } catch (const std::exception&) { return false; }
}

std::string EncodeError(std::string_view code, std::string_view message) {
    return Json{{"v", 1}, {"type", "error"}, {"code", code}, {"message", message}}.dump();
}

bool ParseCommand(std::string_view text, Command* command, std::string* error) {
    if (!command) return false;
    *command = Command{};
    try {
        const auto value = Parse(text);
        const auto op = ReadText(value, "op");
        const std::pair<const char*, CommandOp> ops[] = {
            {"hello", CommandOp::Hello}, {"status", CommandOp::Query}, {"toggle", CommandOp::Toggle},
            {"show", CommandOp::Show}, {"close", CommandOp::Close}, {"send_next", CommandOp::SendNext},
            {"send_all", CommandOp::SendAll}, {"remove_last", CommandOp::RemoveLast},
            {"select_all", CommandOp::SelectAll}, {"paste", CommandOp::Paste}, {"import_clipboard", CommandOp::Paste},
            {"set_insertion", CommandOp::SetInsertion}, {"drag_begin", CommandOp::DragBegin},
            {"drag_end", CommandOp::DragEnd}, {"select_plugin", CommandOp::SelectPlugin},
            {"generate", CommandOp::Generate}, {"cancel_generation", CommandOp::CancelGeneration},
            {"return_to_edit", CommandOp::ReturnToEdit}, {"select_result", CommandOp::SelectResult},
            {"copy_result", CommandOp::CopyResult}, {"cycle_auto_send", CommandOp::CycleAutoSend},
            {"toggle_close_after_last", CommandOp::ToggleCloseAfterLast}};
        Command next;
        for (const auto& [name, code] : ops) if (op == name) next.op = code;
        if (next.op == CommandOp::Unknown) throw std::runtime_error("Unknown operation");
        if (next.op == CommandOp::Paste) {
            next.text = ReadText(value, "text");
            if (next.text.empty()) throw std::runtime_error("Paste requires text");
        }
        if (next.op == CommandOp::SetInsertion) {
            if (!value.at("index").is_number_integer()) throw std::runtime_error("Invalid index");
            const auto index = value.at("index").get<std::int64_t>();
            if (index < 0 || index > 65536) throw std::runtime_error("Invalid index");
            next.insertion_index = static_cast<int>(index);
        }
        if (next.op == CommandOp::SelectPlugin || next.op == CommandOp::SelectResult || next.op == CommandOp::CopyResult) {
            next.id = ReadText(value, "id");
            if (!value.contains("id") || next.id.size() > 256 ||
                (next.op != CommandOp::SelectPlugin && next.id.empty())) throw std::runtime_error("Invalid identity");
        }
        *command = std::move(next);
        if (error) error->clear();
        return true;
    } catch (const std::exception&) {
        if (error) *error = "Invalid Buffer command";
        return false;
    }
}

std::string JsonEscape(std::string_view text) { return Json(text).dump(); }
bool JsonUnescape(std::string_view text, std::string* output) {
    if (!output) return false;
    output->clear();
    try {
        const auto value = Json::parse(std::string("\"") + std::string(text) + "\"");
        if (!value.is_string()) return false;
        *output = value.get<std::string>();
        return true;
    } catch (const std::exception&) { return false; }
}

bool EncodeFrame(std::string_view payload, std::string* frame) {
    if (frame == nullptr || payload.size() > kMaxFrameBytes) {
        return false;
    }
    const auto length = static_cast<std::uint32_t>(payload.size());
    frame->assign(4, '\0');
    (*frame)[0] = static_cast<char>((length >> 24) & 0xFF);
    (*frame)[1] = static_cast<char>((length >> 16) & 0xFF);
    (*frame)[2] = static_cast<char>((length >> 8) & 0xFF);
    (*frame)[3] = static_cast<char>(length & 0xFF);
    frame->append(payload.data(), payload.size());
    return true;
}

bool DecodeFrameHeader(const char header[4], std::uint32_t* length) {
    if (header == nullptr || length == nullptr) {
        return false;
    }
    const auto value = (static_cast<std::uint32_t>(static_cast<unsigned char>(header[0])) << 24) |
                       (static_cast<std::uint32_t>(static_cast<unsigned char>(header[1])) << 16) |
                       (static_cast<std::uint32_t>(static_cast<unsigned char>(header[2])) << 8) |
                       static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
    if (value > kMaxFrameBytes) {
        return false;
    }
    *length = value;
    return true;
}

}  // namespace rimes::buffer
