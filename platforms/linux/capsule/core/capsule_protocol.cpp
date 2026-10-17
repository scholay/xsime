#include "capsule_protocol.hpp"
#include "capsule_hash.hpp"
#include "capsule_text.hpp"
#include "capsule_transport.hpp"
#include <map>
#include <nlohmann/json.hpp>
namespace rimes::capsule {
using Json = nlohmann::json;
Snapshot MakeSnapshot(const CapsuleModel& model) {
    Snapshot snapshot;
    snapshot.generation = model.generation(); snapshot.visible = model.visible(); snapshot.armed = model.armed();
    snapshot.password_field = model.password_field(); snapshot.tab = TabRaw(model.tab()); snapshot.query = model.query();
    snapshot.hint = model.hint(); snapshot.last_copied = model.last_copied(); snapshot.copy_seq = model.copy_seq();
    snapshot.selected = model.selected(); snapshot.count = static_cast<int>(model.cards().size());
    snapshot.module = ModuleRaw(model.module()); snapshot.filter = model.filter(); snapshot.managing = model.managing();
    snapshot.loading = model.loading(); snapshot.modules.assign(model.enabled_modules().begin(), model.enabled_modules().end());
    // Bound every transport frame. Keep the selected item in the window.
    const int start = std::max(0, snapshot.selected - 100);
    const int end = std::min(snapshot.count, start + 200);
    snapshot.cards.assign(model.cards().begin() + start, model.cards().begin() + end);
    for (auto& card : snapshot.cards) {
        // Storage retains the entire Unicode title/summary; UI projections are
        // bounded.
        if (card.title.size() > 4096)
            card.title = PreviewOf(card.title);
        if (card.preview.size() > 4096)
            card.preview = PreviewOf(card.preview);
    }
    snapshot.offset = start;
    snapshot.selected -= start;
    if (const auto* card = model.Selected()) {
        snapshot.selected_id = card->id; snapshot.selected_title = card->title.size() > 4096 ? PreviewOf(card->title) : card->title;
        snapshot.selected_preview =
            card->preview.size() > 4096 ? PreviewOf(card->preview) : card->preview;
    }
    return snapshot;
}
std::string EncodeSnapshot(const Snapshot& value) {
    auto s = value;
    s.password_field = s.password_field || s.session_protected;
    if (s.password_field) {
        s.visible = false; s.armed = false; s.query.clear(); s.last_copied.clear(); s.cards.clear();
        s.selected_id.clear(); s.selected_title.clear(); s.selected_preview.clear(); s.count = 0; s.selected = 0; s.offset = 0;
        s.hint = "受保护输入，内容已隐藏";
        s.detail = false;
        s.detail_notice.clear();
        s.detail_rows.clear();
        s.detail_headings.clear();
        s.detail_revision.clear();
        s.detail_count = 0;
        s.detail_offset = 0;
        s.detail_selected = -1;
    }
    if (s.password_field || !s.visible || (s.module != "temporary" && s.module != "resources" && s.module != "capture")) {
        s.restore_id.clear();
        s.restore_revision.clear();
    }
    if (s.password_field)
        s.clipboard_capture = false;
    Json out{{"v",1},{"generation",s.generation},{"visible",s.visible},{"armed",s.armed},{"password_field",s.password_field},
        {"theme",s.theme},{"tab",s.tab},{"query",s.query},{"hint",s.hint},{"last_copied",s.last_copied},{"copy_seq",s.copy_seq},
        {"selected_id",s.selected_id},{"selected_title",s.selected_title},{"selected_preview",s.selected_preview},
        {"selected",s.selected},{"offset",s.offset},{"count",s.count},{"ui_clients",s.ui_clients},{"module",s.module},{"filter",s.filter},
        {"managing",s.managing},{"loading",s.loading},{"modules",Json::array()},{"cards",Json::array()}};
    out["session_protected"] = s.session_protected;
    out["clipboard_capture"] = s.clipboard_capture;
    out["restore_id"] = s.restore_id;
    out["restore_revision"] = s.restore_revision;
    out["restore_seq"] = s.restore_seq;
    out["restore_close"] = s.restore_close;
    for (auto module : s.modules) out["modules"].push_back(ModuleRaw(module));
    for (const auto& card : s.cards)
        out["cards"].push_back({{"id", card.id},
                                {"kind", KindRaw(card.kind)},
                                {"title", card.title},
                                {"preview", card.preview},
                                {"revision", card.revision},
                                {"module", ModuleRaw(card.module)},
                                {"filter", card.filter},
                                {"favorite", card.favorite},
                                {"source", card.source}});
    out["detail_notice"] = s.detail_notice;
    out["detail"] = s.detail;
    out["detail_revision"] = s.detail_revision;
    out["detail_selected"] = s.detail_selected;
    out["detail_count"] = s.detail_count;
    out["detail_offset"] = s.detail_offset;
    out["detail_rows"] = Json::array();
    for (std::size_t i = 0; i < s.detail_rows.size(); ++i) {
        const auto& row = s.detail_rows[i];
        out["detail_rows"].push_back(
            {{"type", row.type},
             {"label", row.label},
             {"value", row.value},
             {"heading", i < s.detail_headings.size() ? s.detail_headings[i] : ""}});
    }
    return out.dump();
}
bool DecodeSnapshot(std::string_view json, Snapshot* out) {
    if (!out || json.size() > kMaxFrameBytes) return false;
    try {
        const auto data = Json::parse(json);
        if (!data.is_object() || !data.contains("visible") || data.value("v",1) != 1) return false;
        Snapshot s;
        s.generation = data.value("generation", std::uint64_t{0}); s.visible = data.at("visible");
        s.armed = data.value("armed",false); s.password_field = data.value("password_field",false);
        // Old hosts do not distinguish these reasons; keep their fail-closed behavior.
        s.session_protected = data.value("session_protected", s.password_field);
        s.password_field = s.password_field || s.session_protected;
        s.theme = data.value("theme", "night");
        if (s.theme != "day" && s.theme != "quiet" && s.theme != "rasta" && s.theme != "liquidGlass") s.theme = "night";
        s.tab = data.value("tab", "note"); s.query = data.value("query", ""); s.hint = data.value("hint", "");
        s.last_copied = data.value("last_copied", ""); s.copy_seq = data.value("copy_seq",std::uint64_t{0});
        s.selected_id = data.value("selected_id", ""); s.selected_title = data.value("selected_title", "");
        s.selected_preview = data.value("selected_preview", ""); s.selected = data.value("selected",0);
        s.count = data.value("count",0); s.ui_clients = data.value("ui_clients",0);
        s.offset = data.value("offset",0);
        if (s.offset < 0 || s.count < 0 ||
            s.count > (data.value("module", "notes") == "temporary" ? 100000 : kMaxRecords) ||
            s.offset > s.count)
            return false;
        s.module = data.value("module","notes"); s.filter = data.value("filter","all");
        const auto module = ModuleFromRaw(s.module);
        if (!module || !ValidFilter(*module,s.filter)) return false;
        s.clipboard_capture = data.value("clipboard_capture", false);
        s.restore_id = data.value("restore_id", "");
        s.restore_revision = data.value("restore_revision", "");
        s.restore_seq = data.value("restore_seq", std::uint64_t{0});
        s.restore_close = data.value("restore_close", false);
        if (!s.restore_id.empty() &&
            (!LooksLikeUuid(s.restore_id) || s.restore_revision.size() != 64 ||
             s.restore_revision.find_first_not_of("0123456789abcdef") != s.restore_revision.npos))
            return false;
        s.managing = data.value("managing",false); s.loading = data.value("loading",false);
        if (data.contains("modules")) {
            s.modules.clear();
            for (const auto& raw : data.at("modules")) {
                const auto entry = ModuleFromRaw(raw.get<std::string>());
                if (!entry) return false;
                s.modules.push_back(*entry);
            }
            if (s.modules.size() > 5) return false;
        }
        if (data.contains("cards")) {
            if (!data.at("cards").is_array() || data.at("cards").size() > 200) return false;
            for (const auto& item : data.at("cards")) {
                Card card;
                card.id = item.at("id"); card.kind = KindFromRaw(item.at("kind").get<std::string>());
                card.title = item.at("title"); card.preview = item.at("preview");
                card.revision = item.value("revision", ""); card.filter = item.value("filter", "other");
                card.favorite = item.value("favorite", false);
                card.source = item.value("source", "");
                if (card.source.size() > 512)
                    return false;
                card.module = ModuleFromRaw(item.value("module",s.module)).value_or(*module);
                if (!LooksLikeUuid(card.id) || card.title.size() > 4096 ||
                    CharacterCount(card.title) > kMaxTitleChars || card.preview.size() > 4096) return false;
                s.cards.push_back(std::move(card));
            }
        }
        s.detail_notice = data.value("detail_notice", "");
        if (s.detail_notice.size() > 4096)
            return false;
        s.detail = data.value("detail", false);
        s.detail_revision = data.value("detail_revision", "");
        s.detail_count = data.value("detail_count", 0);
        s.detail_offset = data.value("detail_offset", 0);
        s.detail_selected = data.value("detail_selected", -1);
        if (s.detail_count < 0 || s.detail_count > kMaxDetailRows || s.detail_offset < 0 ||
            s.detail_offset > s.detail_count || s.detail_selected < -1 ||
            s.detail_selected >= s.detail_count)
            return false;
        if (data.contains("detail_rows")) {
            if (!data.at("detail_rows").is_array() || data.at("detail_rows").size() > 200)
                return false;
            for (const auto& row : data.at("detail_rows")) {
                DetailRow value{row.at("type"), row.at("label"), row.at("value"), false};
                const auto heading = row.at("heading").get<std::string>();
                if (value.label.size() > 4096 || value.value.size() > 4096 || heading.size() > 4096)
                    return false;
                s.detail_rows.push_back(std::move(value));
                s.detail_headings.push_back(heading);
            }
        }
        if (s.detail_offset + static_cast<int>(s.detail_rows.size()) > s.detail_count)
            return false;
        if (s.detail &&
            (s.detail_revision.size() != 64 ||
             s.detail_revision.find_first_not_of("0123456789abcdef") != s.detail_revision.npos ||
             !LooksLikeUuid(s.selected_id)))
            return false;
        if (!s.visible || (s.module != "temporary" && s.module != "resources" && s.module != "capture") || s.password_field) {
            s.restore_id.clear();
            s.restore_revision.clear();
        }
        if (s.password_field) {
            s.clipboard_capture = false;
            s.visible = false;
            s.armed = false;
            s.query.clear();
            s.last_copied.clear();
            s.cards.clear();
            s.count = 0;
            s.offset = 0;
            s.selected = 0;
            s.selected_id.clear();
            s.selected_title.clear();
            s.selected_preview.clear();
            s.detail = false;
            s.detail_notice.clear();
            s.detail_revision.clear();
            s.detail_rows.clear();
            s.detail_headings.clear();
            s.detail_count = 0;
            s.detail_offset = 0;
            s.detail_selected = -1;
        }
        *out = std::move(s); return true;
    } catch (const std::exception&) { return false; }
}
bool ParseCommand(std::string_view json, Command* out, std::string* error) {
    if (!out || json.size() > 8192) return false;
    try {
        const auto data = Json::parse(json);
        if (!data.is_object() || data.value("v",0) != 1) return false;
        for (const auto& [key,value] : data.items()) {
            (void)value;
            if (key != "v" && key != "op" && key != "text" && key != "tab" && key != "index" && key != "id" && key != "revision") return false;
        }
        static const std::map<std::string, CommandOp> ops{
            {"hello", CommandOp::Hello},
            {"status", CommandOp::Status},
            {"toggle", CommandOp::Toggle},
            {"show", CommandOp::Show},
            {"close", CommandOp::Close},
            {"next", CommandOp::Next},
            {"prev", CommandOp::Prev},
            {"select", CommandOp::Select},
            {"tab", CommandOp::Tab},
            {"activate", CommandOp::Activate},
            {"copy", CommandOp::Copy},
            {"search", CommandOp::Search},
            {"module", CommandOp::Module},
            {"filter", CommandOp::Filter},
            {"manage", CommandOp::Manage},
            {"rail", CommandOp::Rail},
            {"refresh", CommandOp::Refresh},
            {"favorite", CommandOp::Favorite},
            {"remove", CommandOp::Remove},
            {"clear-history", CommandOp::ClearHistory},
            {"detail", CommandOp::Detail},
            {"detail-close", CommandOp::DetailClose},
            {"row-select", CommandOp::RowSelect},
            {"row-copy", CommandOp::RowCopy},
            {"row-insert", CommandOp::RowInsert}};
        const auto found = ops.find(data.at("op").get<std::string>());
        if (found == ops.end()) return false;
        Command command;
        command.op = found->second; command.text = data.value("text", data.value("tab", ""));
        command.index = data.value("index",-1); command.id = data.value("id", "");
        command.revision = data.value("revision", "");
        if (!command.revision.empty() &&
            (command.revision.size() != 64 ||
             command.revision.find_first_not_of("0123456789abcdef") != command.revision.npos))
            return false;
        if ((command.op == CommandOp::Favorite || command.op == CommandOp::Remove) &&
            (command.id.empty() || command.revision.empty()))
            return false;
        if ((command.op == CommandOp::RowSelect || command.op == CommandOp::RowCopy ||
             command.op == CommandOp::RowInsert) &&
            (command.id.empty() || command.revision.empty() || command.index < 0))
            return false;
        if (command.text.size() > 4096 || command.text.find('\0') != command.text.npos ||
            (!command.id.empty() && !LooksLikeUuid(command.id)) || command.index < -1 || command.index > kMaxDetailRows) return false;
        if (command.op == CommandOp::Module && !ModuleFromRaw(command.text)) return false;
        *out = std::move(command); return true;
    } catch (const std::exception&) { if (error) *error = "Invalid Capsule command"; return false; }
}
} // namespace rimes::capsule
