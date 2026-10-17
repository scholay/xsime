#include "capsule_hash.hpp"
#include "capsule_rich_text.hpp"
#include "capsule_store.hpp"
#include "capsule_structure.hpp"
#include "capsule_text.hpp"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
using namespace rimes::capsule;
using Json = nlohmann::json;
void Check(bool test, const std::string& message) {
    if (!test)
        throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    try {
        std::ifstream stream(NATIVE_NOTES_FIXTURES);
        const auto fixture = Json::parse(stream);
        for (const auto& item : fixture.at("characters")) {
            const auto text = item.at("text").get<std::string>();
            Check(CharacterCount(text) == item.at("count").get<std::size_t>(),
                  "native grapheme count");
            Check(CharacterPrefix(text, 1) == item.at("prefix"), "native grapheme boundary");
        }
        for (const auto& item : fixture.at("grammar")) {
            const auto body = item.at("body").get<std::string>();
            Json actual = Json::array();
            for (const auto& section : NoteSections(body)) {
                Json rows = Json::array();
                for (const auto& row : section.rows)
                    rows.push_back({{"type", row.type},
                                    {"label", row.label},
                                    {"value", row.value},
                                    {"secret", row.secret}});
                actual.push_back({{"heading", section.heading}, {"rows", rows}});
            }
            Check(actual == item.at("sections"),
                  "native grammar mismatch: " + body + "\n" + actual.dump());
            if (!item.at("summary").is_null())
                Check(NoteSummary(body) == item.at("summary"), "native summary");
        }
        for (const auto& item : fixture.at("summaries"))
            Check(NormalizeSummary(item.at("input").get<std::string>()) == item.at("normalized"),
                  "native summary normalization");
        for (const auto& item : fixture.at("headers")) {
            const auto parsed = ParseFrontMatter(item.at("lines").get<std::vector<std::string>>());
            Check(parsed.has_value() == item.at("valid"), "native header validity");
            if (!parsed)
                continue;
            Check(Json(parsed->Preserved()) == item.at("preserved"), "native property round trip");
            Json fields = Json::array();
            for (const auto& row : parsed->Fields())
                fields.push_back(
                    {{"label", row.label}, {"value", row.value}, {"secret", row.secret}});
            Check(fields == item.at("fields"), "native header fields");
        }
        for (const auto& item : fixture.at("bullets")) {
            const auto line = item.at("line").get<std::string>();
            auto toggled = ToggleBullet(line);
            Check((toggled ? Json(*toggled) : Json(nullptr)) == item.at("toggle"),
                  "native checkbox toggle");
            Check(IndentBullet(line, false) == item.at("indent") &&
                      IndentBullet(line, true) == item.at("outdent"),
                  "native indent");
        }
        Json exported = Json::array();
        for (const auto& item : fixture.at("rich")) {
            const auto original = Base64Decode(item.at("asset").get<std::string>());
            Check(original.has_value(), "native base64");
            const auto decoded = ReadRichText(*original);
            Check(decoded.has_value(), "native RTFD decode");
            Check(decoded->editable, "native example unexpectedly unsupported");
            Check(decoded->PlainText() == item.at("text"),
                  "native RTFD text: " + decoded->PlainText());
            Check(decoded->Markdown() == item.at("markdown"),
                  "native RTFD projection: " + decoded->Markdown());
            const auto encoded = WriteRichText(*decoded);
            Check(encoded.has_value(), "RTFD encode");
            const auto again = ReadRichText(*encoded);
            Check(again && again->PlainText() == decoded->PlainText() &&
                      again->Markdown() == decoded->Markdown() &&
                      again->assets == decoded->assets &&
                      again->runs.size() == decoded->runs.size(),
                  "RTFD round trip");
            for (std::size_t i = 0; i < again->runs.size(); ++i)
                Check(again->runs[i].SameStyle(decoded->runs[i]),
                      "RTFD image geometry/style changed");
            Json assets = Json::array();
        for (const auto& [name, bytes] : decoded->assets) assets.push_back(Base64Encode(bytes));
        exported.push_back({{"name", item.at("name")},
                                {"asset", Base64Encode(*encoded)},
                                {"text", decoded->PlainText()},
                                {"markdown", decoded->Markdown()}, {"attachments", assets}});
            for (std::size_t size = 0; size < original->size(); ++size)
                Check(!ReadRichText(std::string_view(*original).substr(0, size)),
                      "truncated RTFD accepted");
        }
        for (const auto* text : {"%%%x", "AA=A", "A===", "A", "AAAA===", "ZE=="})
            Check(!Base64Decode(text), "invalid base64 accepted");
        if (argc == 2) {
            std::ofstream output(argv[1]);
            output << exported.dump(2) << '\n';
        }
        std::cout << "macOS fixtures: Unicode, structured notes, front matter, "
                     "Bullet, RTFD and asset round trips passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
