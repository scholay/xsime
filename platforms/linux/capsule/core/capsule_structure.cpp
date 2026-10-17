#include "capsule_structure.hpp"
#include "capsule_text.hpp"
#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <unicode/uchar.h>
#include <unicode/unistr.h>
#include <unicode/utf8.h>
namespace rimes::capsule {
namespace {
const std::set<std::string> reserved = {"capsule",
                                        "version",
                                        "id",
                                        "title",
                                        "updated_at",
                                        "summary",
                                        "sync_asset",
                                        "sync_project",
                                        "capsule_note_format",
                                        "capsule_resource_type",
                                        "capsule_rich_version",
                                        "capsule_rich_hash",
                                        "capsule_projection_hash",
                                        "capsule_rich_asset",
                                        "capsule_linux_asset"};
const std::set<std::string> metadata = {"tags",  "tag",        "aliases",
                                        "alias", "cssclasses", "cssclass"};
bool Starts(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}
std::optional<std::pair<std::string, std::string>> KeyValue(std::string_view line) {
    if (line.empty())
        return {};
    if (line[0] == '\'' || line[0] == '"') {
        const auto end = line.find(line[0], 1);
        if (end == line.npos || end + 1 >= line.size() || line[end + 1] != ':')
            return {};
        return std::make_pair(std::string(line.substr(1, end - 1)), TrimText(line.substr(end + 2)));
    }
    for (std::size_t at = 0; at < line.size(); ++at)
        if (line[at] == ':' &&
            (at + 1 == line.size() || line[at + 1] == ' ' || line[at + 1] == '\t'))
            return std::make_pair(TrimText(line.substr(0, at)), TrimText(line.substr(at + 1)));
    return {};
}
std::optional<std::string> Item(std::string_view line) {
    std::size_t start = 0;
    if (line.size() >= 2 && std::string_view("-*+").find(line[0]) != std::string_view::npos &&
        line[1] == ' ')
        start = 2;
    else {
        int digits = 0;
        int32_t offset = 0;
        while (offset < static_cast<int32_t>(line.size())) {
            const auto previous = offset;
            UChar32 code;
            U8_NEXT(line.data(), offset, static_cast<int32_t>(line.size()), code);
            if (code < 0 || u_getNumericValue(code) == U_NO_NUMERIC_VALUE) {
                offset = previous;
                break;
            }
            if (++digits > 3)
                return {};
        }
        start = offset;
        if (!digits || start + 1 >= line.size() || (line[start] != '.' && line[start] != ')') ||
            line[start + 1] != ' ')
            return {};
        start += 2;
    }
    auto value = line.substr(start);
    for (const auto box : {"[ ] ", "[x] ", "[X] "})
        if (Starts(value, box)) {
            value.remove_prefix(4);
            break;
        }
    return TrimText(value);
}
std::optional<DetailRow> Field(std::string_view item) {
    auto colon = item.find(':');
    const auto wide = item.find("：");
    std::size_t length = 1;
    if (wide != item.npos && (colon == item.npos || wide < colon)) {
        colon = wide;
        length = 3;
    }
    if (colon == item.npos)
        return {};
    auto label = TrimText(item.substr(0, colon)), value = TrimText(item.substr(colon + length));
    if (label.empty() || value.empty() || CharacterCount(label) > 24 || Starts(value, "//"))
        return {};
    return DetailRow{"field", label, value, SecretLabel(label)};
}
std::string Clip(std::string_view text) {
    const auto trimmed = TrimText(text);
    return CharacterCount(trimmed) > 120 ? CharacterPrefix(trimmed, 119) + "…" : trimmed;
}
} // namespace
std::optional<std::string> DecodeScalar(std::string_view text) {
    const auto raw = TrimText(text);
    if (raw.empty())
        return std::string{};
    if (raw[0] == '"') {
        try {
            auto parsed = nlohmann::json::parse(raw);
            if (parsed.is_string())
                return parsed.get<std::string>();
        } catch (const std::exception&) {
        }
        return {};
    }
    if (raw[0] == '\'') {
        if (raw.size() < 2 || raw.back() != '\'')
            return {};
        auto out = raw.substr(1, raw.size() - 2);
        std::size_t index = 0;
        while ((index = out.find("''", index)) != out.npos) {
            out.erase(index, 1);
            ++index;
        }
        return out;
    }
    if (std::string_view("[{|>&*!%@`").find(raw[0]) != std::string_view::npos)
        return {};
    const auto comment = raw.find(" #");
    return comment == raw.npos ? raw : TrimText(raw.substr(0, comment));
}
std::optional<FrontMatter> ParseFrontMatter(const std::vector<std::string>& lines) {
    if (lines.size() > 20000)
        return {};
    FrontMatter result;
    std::set<std::string> seen;
    HeaderEntry* pending = nullptr;
    for (const auto& line : lines) {
        (void)CharacterCount(line);
        if (!line.empty() &&
            (line[0] == ' ' || line[0] == '\t' || Starts(line, "- ") || line == "-")) {
            if (!pending)
                return {};
            pending->lines.push_back(line);
            pending->scalar.reset();
            continue;
        }
        pending = nullptr;
        const auto trimmed = TrimText(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            result.entries.push_back({"", {line}, {}});
            continue;
        }
        auto entry = KeyValue(line);
        if (!entry || entry->first.empty() || !seen.insert(entry->first).second)
            return {};
        result.entries.push_back({entry->first, {line}, DecodeScalar(entry->second)});
        pending = &result.entries.back();
    }
    return result;
}
std::optional<std::string> FrontMatter::Scalar(std::string_view key) const {
    for (const auto& entry : entries)
        if (entry.key == key)
            return entry.scalar;
    return {};
}
std::vector<std::string> FrontMatter::Preserved() const {
    std::vector<std::string> result;
    for (const auto& entry : entries)
        if (!entry.key.empty() && !reserved.count(entry.key))
            result.insert(result.end(), entry.lines.begin(), entry.lines.end());
    return result;
}
std::vector<DetailRow> FrontMatter::Fields() const {
    std::vector<DetailRow> result;
    for (const auto& entry : entries)
        if (!entry.key.empty() && !reserved.count(entry.key) &&
            !metadata.count(LowerText(entry.key)) && entry.scalar && !entry.scalar->empty())
            result.push_back({"field", entry.key, *entry.scalar, SecretLabel(entry.key)});
    return result;
}
bool SecretLabel(std::string_view label) {
    const auto lowered = LowerText(label);
    for (const auto phrase :
         {"密码", "口令", "密钥", "私钥", "秘钥", "助记词", "恢复短语", "恢复码", "安全码",
          "api key", "apikey", "access key", "secret key", "private key"})
        if (lowered.find(phrase) != lowered.npos)
            return true;
    const std::set<std::string> words = {"password", "passwd", "pwd",     "pass", "passcode",
                                         "pin",      "secret", "token",   "key",  "cvv",
                                         "cvc",      "seed",   "mnemonic"};
    std::string token;
    for (unsigned char c : lowered + " ") {
        if (std::isalnum(c) || c >= 128)
            token += static_cast<char>(c);
        else {
            if (words.count(token))
                return true;
            token.clear();
        }
    }
    return false;
}
std::vector<DetailSection> NoteSections(std::string_view body,
                                        const std::vector<DetailRow>& headers) {
    std::vector<DetailSection> sections;
    DetailSection current{"", headers};
    bool fenced = false;
    char marker = 0;
    std::string block;
    auto flush = [&] {
        if (!current.heading.empty() || !current.rows.empty())
            sections.push_back(std::move(current));
        current = {};
    };
    std::istringstream lines{std::string(body)};
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto trimmed = TrimText(line);
        if (fenced) {
            if (trimmed.size() >= 3 &&
                std::all_of(trimmed.begin(), trimmed.end(), [&](char c) { return c == marker; })) {
                if (!block.empty())
                    block.pop_back();
                current.rows.push_back({"code", "", block, false});
                block.clear();
                fenced = false;
            } else
                block += line + '\n';
            continue;
        }
        if (Starts(trimmed, "```") || Starts(trimmed, "~~~")) {
            fenced = true;
            marker = trimmed[0];
            continue;
        }
        if (trimmed.empty())
            continue;
        auto hashes = trimmed.find_first_not_of('#');
        if (hashes >= 1 && hashes <= 6 && hashes < trimmed.size() && trimmed[hashes] == ' ') {
            auto title = TrimText(trimmed.substr(hashes));
            if (!title.empty()) {
                flush();
                current.heading = std::move(title);
                continue;
            }
        }
        const auto item = Item(trimmed);
        const auto field = item ? Field(*item) : std::optional<DetailRow>{};
        if (field)
            current.rows.push_back(*field);
        else {
            auto text = item.value_or(Starts(trimmed, ">") ? TrimText(trimmed.substr(1)) : trimmed);
            if (!text.empty())
                current.rows.push_back({"line", "", std::move(text), false});
        }
    }
    if (fenced && !body.empty() && body.back() == '\n')
        block += '\n';
    if (fenced && !block.empty()) {
        block.pop_back();
        current.rows.push_back({"code", "", std::move(block), false});
    }
    flush();
    return sections;
}
std::vector<DetailRow> FlattenSections(const std::vector<DetailSection>& sections) {
    std::vector<DetailRow> rows;
    for (const auto& section : sections)
        rows.insert(rows.end(), section.rows.begin(), section.rows.end());
    return rows;
}
std::string NoteSummary(std::string_view body) {
    for (const auto& section : NoteSections(body)) {
        for (const auto& row : section.rows) {
            if (row.type == "line")
                return Clip(row.value);
            if (row.type == "field" && !row.secret)
                return Clip(row.label + " " + row.value);
        }
        if (!section.heading.empty())
            return Clip(section.heading);
    }
    const auto flat = OneLine(body);
    return CharacterCount(flat) > 72 ? CharacterPrefix(flat, 72) + "…" : flat;
}
std::string NormalizeSummary(std::string_view summary) {
    (void)CharacterCount(summary);
    auto text = icu::UnicodeString::fromUTF8(icu::StringPiece(summary.data(), summary.size()));
    std::string line, result;
    auto flush = [&] {
        auto trimmed = TrimText(line);
        if (!trimmed.empty()) {
            if (!result.empty())
                result += ' ';
            result += trimmed;
        }
        line.clear();
    };
    for (int32_t i = 0; i < text.length();) {
        const auto code = text.char32At(i);
        i = text.moveIndex32(i, 1);
        if ((code >= 0x0a && code <= 0x0d) || code == 0x85 || code == 0x2028 || code == 0x2029)
            flush();
        else {
            icu::UnicodeString character;
            character.append(code);
            character.toUTF8String(line);
        }
    }
    flush();
    return CharacterPrefix(result, 120);
}
std::optional<std::string> ToggleBullet(std::string_view line) {
    const auto start = line.find_first_not_of(" \t");
    if (start == line.npos)
        return {};
    auto body = line.substr(start);
    std::string mark;
    if (Starts(body, "- [ ] "))
        mark = "- [x] ";
    else if (Starts(body, "- [x] ") || Starts(body, "- [X] "))
        mark = "- [ ] ";
    else
        return {};
    return std::string(line.substr(0, start)) + mark + std::string(body.substr(6));
}
std::string IndentBullet(std::string_view line, bool outdent) {
    if (!outdent)
        return "  " + std::string(line);
    return std::string(line.substr(Starts(line, "  ") ? 2 : Starts(line, "\t") ? 1 : 0));
}
} // namespace rimes::capsule
