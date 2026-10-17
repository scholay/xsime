#include "capsule_protocol.hpp"

#include <iostream>
#include <nlohmann/json.hpp>

namespace {

int g_failures = 0;

void Expect(bool cond, const char* message) {
    if (!cond) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

}  // namespace

int main() {
    rimes::capsule::CapsuleModel model;
    rimes::capsule::Record note;
    note.id = rimes::capsule::kDefaultEntryId;
    note.kind = rimes::capsule::Kind::Note;
    note.title = rimes::capsule::kDefaultEntryTitle;
    note.content = rimes::capsule::kDefaultEntryContent;
    model.LoadCards({note});
    model.set_visible(true);
    model.set_armed(true);
    const auto json = rimes::capsule::EncodeSnapshot(rimes::capsule::MakeSnapshot(model));
    Expect(json.find("\"visible\":true") != std::string::npos, "visible");
    Expect(json.find(rimes::capsule::kDefaultEntryTitle) != std::string::npos, "title");
    Expect(json.find("\"payload\"") == std::string::npos, "no full payload on the wire");

    rimes::capsule::Snapshot decoded;
    Expect(rimes::capsule::DecodeSnapshot(json, &decoded), "decode");
    Expect(decoded.visible && decoded.armed, "decoded flags");
    Expect(decoded.cards.size() == 1, "decoded card");
    Expect(decoded.cards[0].title == rimes::capsule::kDefaultEntryTitle, "decoded title");
    auto themed = rimes::capsule::MakeSnapshot(model);
    themed.theme = "rasta";
    Expect(rimes::capsule::DecodeSnapshot(rimes::capsule::EncodeSnapshot(themed), &decoded) && decoded.theme == "rasta", "shared theme propagation");
    Expect(rimes::capsule::DecodeSnapshot(R"({"visible":true})", &decoded) && decoded.theme == "night", "legacy snapshot theme fallback");

    model.set_last_copied("private-clipboard");
    model.set_query("private-query");
    model.set_password_field(true);
    const auto protected_json = rimes::capsule::EncodeSnapshot(rimes::capsule::MakeSnapshot(model));
    Expect(protected_json.find("private-") == std::string::npos, "protected clipboard and query leaked");
    Expect(protected_json.find(rimes::capsule::kDefaultEntryTitle) == std::string::npos, "protected card title leaked");
    Expect(rimes::capsule::DecodeSnapshot(protected_json, &decoded) && decoded.cards.empty() &&
           !decoded.visible && !decoded.armed, "protected snapshot still actionable");

    rimes::capsule::Command command;
    std::string error;
    Expect(rimes::capsule::ParseCommand(R"({"v":1,"op":"activate"})", &command, &error),
           "parse activate");
    Expect(command.op == rimes::capsule::CommandOp::Activate, "activate op");
    Expect(!rimes::capsule::ParseCommand(R"({"v":1,"op":"nope"})", &command, &error),
           "unknown op");

    auto detail = rimes::capsule::MakeSnapshot(model);
    detail.password_field = false;
    detail.visible = true;
    detail.selected_id = note.id;
    detail.detail = true;
    detail.detail_revision = std::string(64, 'a');
    detail.detail_count = 2;
    detail.detail_rows = {{"field", "账号", "row-value", false}, {"code", "", "two\nlines", false}};
    detail.detail_headings = {"登录", "代码"};
    Expect(rimes::capsule::DecodeSnapshot(rimes::capsule::EncodeSnapshot(detail), &decoded) &&
               decoded.detail_rows[1].value == "two\nlines" && decoded.detail_selected == -1,
           "detail frame retains row values and empty selection");
    detail.password_field = true;
    Expect(rimes::capsule::EncodeSnapshot(detail).find("row-value") == std::string::npos,
           "protected detail leaked");
    const auto row = std::string(R"({"v":1,"op":"row-insert","id":")") + note.id +
                     R"(","revision":")" + std::string(64, 'a') + R"(","index":0})";
    Expect(rimes::capsule::ParseCommand(row, &command, &error) &&
               command.op == rimes::capsule::CommandOp::RowInsert,
           "revision-bound detail command");
    Expect(
        !rimes::capsule::ParseCommand(R"({"v":1,"op":"row-insert","index":0})", &command, &error),
        "unbound row insert accepted");
    detail.password_field = false;
    detail.detail_selected = 2;
    Expect(!rimes::capsule::DecodeSnapshot(rimes::capsule::EncodeSnapshot(detail), &decoded),
           "invalid detail selection accepted");
    {
        using namespace rimes::capsule;
        using Json = nlohmann::json;
        Snapshot history;
        history.visible = true;
        history.module = "temporary";
        history.clipboard_capture = true;
        history.count = 100000;
        history.offset = 99999;
        history.selected = 0;
        history.restore_id = note.id;
        history.restore_revision = std::string(64, 'a');
        history.restore_seq = 42;
        history.restore_close = true;
        Card card;
        card.id = note.id;
        card.title = "copy";
        card.module = Module::Temporary;
        card.favorite = true;
        card.source = "Fixture";
        history.cards = {card};
        const auto encoded = EncodeSnapshot(history);
        Expect(DecodeSnapshot(encoded, &decoded) && decoded.clipboard_capture &&
                   decoded.restore_seq == 42 && decoded.restore_close &&
                   decoded.cards.at(0).favorite && decoded.cards.at(0).source == "Fixture",
               "history metadata and restore intent roundtrip");
        history.password_field = true;
        Expect(DecodeSnapshot(EncodeSnapshot(history), &decoded) && !decoded.clipboard_capture &&
                   decoded.restore_id.empty() && decoded.cards.empty() && decoded.count == 0,
               "protected history projection");
        auto peer = Json::parse(encoded);
        peer["password_field"] = true;
        Expect(DecodeSnapshot(peer.dump(), &decoded) && decoded.count == 0 && decoded.offset == 0 &&
                   decoded.restore_id.empty(),
               "decoder must also conceal a protected peer snapshot");
        peer = Json::parse(encoded);
        peer["visible"] = false;
        Expect(DecodeSnapshot(peer.dump(), &decoded) && decoded.restore_id.empty() &&
                   decoded.clipboard_capture,
               "hidden observer may capture but cannot restore");
        peer = Json::parse(encoded);
        peer["restore_revision"] = "bad";
        Expect(!DecodeSnapshot(peer.dump(), &decoded), "invalid restore revision accepted");
        for (const auto* op : {"favorite", "remove"}) {
            const auto bound =
                Json{{"v", 1}, {"op", op}, {"id", note.id}, {"revision", std::string(64, 'a')}};
            Expect(ParseCommand(bound.dump(), &command, &error),
                   "revision-bound history command rejected");
            auto unbound = bound;
            unbound.erase("revision");
            Expect(!ParseCommand(unbound.dump(), &command, &error),
                   "unbound destructive history command accepted");
        }
    }
    if (g_failures != 0) {
        std::cerr << g_failures << " capsule protocol failures\n";
        return 1;
    }
    std::cout << "ok: capsule protocol\n";
    return 0;
}
