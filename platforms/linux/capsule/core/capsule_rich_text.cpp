#include "capsule_rich_text.hpp"
#include "capsule_text.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <openssl/evp.h>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unicode/unistr.h>
namespace rimes::capsule {
namespace {
constexpr std::size_t limit = 4 * 1024 * 1024;
bool SafeName(const std::string& name) {
    return !name.empty() && name.size() <= 255 && name != "." && name != ".." &&
           name.find_first_of("/\\\0\r\n", 0, 5) == name.npos;
}
std::uint32_t Read32(std::string_view bytes, std::size_t& cursor) {
    if (cursor + 4 > bytes.size())
        throw std::runtime_error("truncated RTFD");
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i)
        value |= std::uint32_t(static_cast<unsigned char>(bytes[cursor++])) << (8 * i);
    return value;
}
void Write32(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i)
        out += static_cast<char>(value >> (8 * i));
}
std::string Take(std::string_view bytes, std::size_t& cursor, std::size_t size) {
    if (size > bytes.size() - cursor)
        throw std::runtime_error("truncated RTFD member");
    auto result = std::string(bytes.substr(cursor, size));
    cursor += size;
    return result;
}
std::map<std::string, std::string> Unpack(std::string_view bytes) {
    if (bytes.size() > limit || bytes.substr(0, 4) != "rtfd")
        throw std::runtime_error("invalid RTFD");
    std::size_t cursor = 4;
    if (Read32(bytes, cursor) != 0 || Read32(bytes, cursor) != 3)
        throw std::runtime_error("unsupported RTFD version");
    const auto count = Read32(bytes, cursor);
    if (!count || count > 257)
        throw std::runtime_error("RTFD member limit");
    std::vector<std::string> names;
    std::set<std::string> seen;
    for (std::size_t i = 0; i < count; ++i) {
        const auto size = Read32(bytes, cursor);
        auto name = Take(bytes, cursor, size);
        if ((name != "." && !SafeName(name)) || !seen.insert(name).second)
            throw std::runtime_error("invalid RTFD member");
        names.push_back(std::move(name));
    }
    std::vector<std::uint32_t> sizes;
    for (std::size_t i = 0; i < count; ++i)
        sizes.push_back(Read32(bytes, cursor));
    std::map<std::string, std::string> files;
    for (std::size_t i = 0; i < count; ++i) {
        if (sizes[i] < 8 || sizes[i] > bytes.size() - cursor)
            throw std::runtime_error("invalid RTFD size");
        const auto type = Read32(bytes, cursor), size = Read32(bytes, cursor);
        if (type != 1 || size != sizes[i] - 8)
            throw std::runtime_error("unsupported RTFD member type");
        auto data = Take(bytes, cursor, size);
        if (names[i] != ".")
            files.emplace(names[i], std::move(data));
    }
    if (cursor != bytes.size() || !files.count("TXT.rtf"))
        throw std::runtime_error("invalid RTFD archive");
    return files;
}
std::string Pack(std::map<std::string, std::string> files) {
    std::string attributes;
    Write32(attributes, files.size());
    for (const auto& [name, bytes] : files) {
        (void)bytes;
        Write32(attributes, name.size());
        attributes += name;
    }
    for (std::size_t i = 0; i < files.size(); ++i)
        Write32(attributes, 16);
    for (std::size_t i = 0; i < files.size(); ++i) {
        Write32(attributes, 0);
        Write32(attributes, 0600);
        Write32(attributes, 0);
        Write32(attributes, 0);
    }
    files.emplace(".", std::move(attributes));
    std::string out = "rtfd";
    Write32(out, 0);
    Write32(out, 3);
    Write32(out, files.size());
    for (const auto& [name, bytes] : files) {
        (void)bytes;
        Write32(out, name.size());
        out += name;
    }
    for (const auto& [name, bytes] : files) {
        (void)name;
        Write32(out, bytes.size() + 8);
    }
    for (const auto& [name, bytes] : files) {
        (void)name;
        Write32(out, 1);
        Write32(out, bytes.size());
        out += bytes;
    }
    if (out.size() > limit)
        throw std::runtime_error("RTFD size limit");
    return out;
}
void AppendCode(std::string& out, std::uint32_t code) {
    if (!code || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
        throw std::runtime_error("invalid Unicode in RTF");
    icu::UnicodeString character;
    character.append(UChar32(code));
    character.toUTF8String(out);
}
struct State {
    RichRun style;
    bool skip = false;
    int uc = 1;
    int image_index = -1;
    std::string* capture = nullptr;
};
class Reader {
    std::string_view source;
    std::size_t cursor = 0;
    RichDocument document;
    int pending_high = 0, fallback = 0;
    bool attachment_byte = false;
    void Emit(std::string text, State& state, const std::string* link) {
        if (state.skip)
            return;
        if (state.capture) {
            *state.capture += text;
            return;
        }
        auto run = state.style;
        run.text = std::move(text);
        if (link)
            run.link = *link;
        if (!document.runs.empty() && document.runs.back().SameStyle(run) && run.asset.empty())
            document.runs.back().text += run.text;
        else
            document.runs.push_back(std::move(run));
    }
    void Code(std::uint32_t code, State& state, const std::string* link) {
        if (state.skip)
            return;
        if (code >= 0xd800 && code <= 0xdbff) {
            if (pending_high)
                throw std::runtime_error("bad surrogate");
            pending_high = code;
            return;
        }
        if (code >= 0xdc00 && code <= 0xdfff) {
            if (!pending_high)
                throw std::runtime_error("bad surrogate");
            code = 0x10000 + ((pending_high - 0xd800) << 10) + (code - 0xdc00);
            pending_high = 0;
        } else if (pending_high)
            throw std::runtime_error("bad surrogate");
        std::string text;
        AppendCode(text, code);
        Emit(std::move(text), state, link);
    }
    void Byte(unsigned char value, State& state, const std::string* link) {
        if (fallback) {
            --fallback;
            return;
        }
        if (attachment_byte && value == 0xac) {
            attachment_byte = false;
            return;
        }
        attachment_byte = false;
        static constexpr std::uint32_t table[] = {
            0x20ac, 0x81,   0x201a, 0x192,  0x201e, 0x2026, 0x2020, 0x2021, 0x2c6,  0x2030, 0x160,
            0x2039, 0x152,  0x8d,   0x17d,  0x8f,   0x90,   0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
            0x2013, 0x2014, 0x2dc,  0x2122, 0x161,  0x203a, 0x153,  0x9d,   0x17e,  0x178};
        Code(value >= 0x80 && value <= 0x9f ? table[value - 0x80] : value, state, link);
    }
    void Group(State state, int depth, std::string* link) {
        if (depth > 64 || cursor >= source.size() || source[cursor++] != '{')
            throw std::runtime_error("invalid RTF grouping");
        std::string field_link, instruction;
        bool field = false, collect_instruction = false, star = false;
        while (cursor < source.size()) {
            unsigned char ch = source[cursor++];
            if (ch == '}') {
                if (collect_instruction && link) {
                    auto start = instruction.find("HYPERLINK ");
                    auto first = instruction.find('"', start), last = instruction.rfind('"');
                    if (start == std::string::npos || first == std::string::npos || last <= first)
                        document.editable = false;
                    else
                        *link = instruction.substr(first + 1, last - first - 1);
                }
                return;
            }
            if (ch == '{') {
                --cursor;
                Group(state, depth + 1, field ? &field_link : link);
                continue;
            }
            if (ch == '\n' || ch == '\r')
                continue;
            if (ch != '\\') {
                Byte(ch, state, field ? &field_link : link);
                continue;
            }
            if (cursor >= source.size())
                throw std::runtime_error("truncated RTF control");
            ch = source[cursor++];
            if (ch == '\\' || ch == '{' || ch == '}') {
                Byte(ch, state, link);
                continue;
            }
            if (ch == '\n' || ch == '\r') {
                if (ch == '\r' && cursor < source.size() && source[cursor] == '\n')
                    ++cursor;
                Code('\n', state, link);
                continue;
            }
            if (ch == '*') {
                star = true;
                continue;
            }
            if (ch == '\'') {
                if (cursor + 2 > source.size())
                    throw std::runtime_error("bad hex escape");
                auto hex = [](unsigned char c) -> int {
                    return c >= '0' && c <= '9'   ? c - '0'
                           : c >= 'a' && c <= 'f' ? c - 'a' + 10
                           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                  : -1;
                };
                int a = hex(source[cursor]), b = hex(source[cursor + 1]);
                cursor += 2;
                if (a < 0 || b < 0)
                    throw std::runtime_error("bad hex escape");
                Byte(a * 16 + b, state, link);
                continue;
            }
            if (ch == '~') {
                Code(0xa0, state, link);
                continue;
            }
            if (ch == '_') {
                Code(0x2011, state, link);
                continue;
            }
            if (ch == '-') {
                Code(0xad, state, link);
                continue;
            }
            if (!std::isalpha(ch))
                continue;
            std::string word(1, ch);
            while (cursor < source.size() &&
                   std::isalpha(static_cast<unsigned char>(source[cursor])))
                word += source[cursor++];
            if (word.size() > 64)
                throw std::runtime_error("RTF control limit");
            int sign = 1;
            if (cursor < source.size() && source[cursor] == '-') {
                sign = -1;
                ++cursor;
            }
            int value = 0;
            bool number = false;
            while (cursor < source.size() &&
                   std::isdigit(static_cast<unsigned char>(source[cursor]))) {
                number = true;
                if (value > 10000000)
                    throw std::runtime_error("RTF number limit");
                value = value * 10 + (source[cursor++] - '0');
            }
            value *= sign;
            if (cursor < source.size() && source[cursor] == ' ')
                ++cursor;
            if (star && word != "fldinst")
                state.skip = true;
            star = false;
            if (word == "fonttbl" || word == "colortbl" || word == "stylesheet" || word == "info" ||
                word == "header" || word == "footer")
                state.skip = true;
            if (word == "field") {
                field = true;
                continue;
            }
            if (word == "fldinst") {
                collect_instruction = true;
                state.capture = &instruction;
                continue;
            }
            if (word == "NeXTGraphic" && !state.skip) {
                const auto start = cursor;
                while (cursor < source.size() && source[cursor] != '\\' && source[cursor] != '}')
                    ++cursor;
                const auto name = TrimText(source.substr(start, cursor - start));
                if (!document.assets.count(name))
                    throw std::runtime_error("missing RTFD image");
                auto run = state.style;
                run.text.clear();
                run.asset = name;
                document.runs.push_back(std::move(run));
                state.image_index = static_cast<int>(document.runs.size()) - 1;
                state.skip = true;
                attachment_byte = true;
                continue;
            }
            if (word == "bin") {
                if (!number || value < 0 ||
                    static_cast<std::size_t>(value) > source.size() - cursor)
                    throw std::runtime_error("invalid binary data");
                cursor += value;
                if (!state.skip)
                    document.editable = false;
                continue;
            }
            if (state.image_index >= 0 && (word == "width" || word == "height")) {
                if (!number || value < 0 || value > 1000000)
                    document.editable = false;
                else if (word == "width")
                    document.runs[state.image_index].width_twips = value;
                else
                    document.runs[state.image_index].height_twips = value;
            }
            if (state.skip)
                continue;
            if (word == "object" || word == "pict" || word == "trowd") {
                document.editable = false;
                state.skip = word != "trowd";
                continue;
            }
            if (word == "ansicpg" && value != 1252)
                document.editable = false;
            if (word == "uc") {
                if (value < 0 || value > 16)
                    throw std::runtime_error("RTF fallback limit");
                state.uc = value;
            } else if (word == "u") {
                Code(static_cast<std::uint16_t>(value), state, link);
                fallback = state.uc;
            } else if (word == "par" || word == "line")
                Code('\n', state, link);
            else if (word == "tab")
                Code('\t', state, link);
            else if (word == "b")
                state.style.bold = !number || value != 0;
            else if (word == "i")
                state.style.italic = !number || value != 0;
            else if (word == "ul")
                state.style.underline = !number || value != 0;
            else if (word == "ulnone")
                state.style.underline = false;
            else if (word == "strike")
                state.style.strike = !number || value != 0;
            else if (word == "plain")
                state.style = RichRun{};
            else if (word == "emdash")
                Code(0x2014, state, link);
            else if (word == "endash")
                Code(0x2013, state, link);
            else if (word == "lquote")
                Code(0x2018, state, link);
            else if (word == "rquote")
                Code(0x2019, state, link);
            else if (word == "ldblquote")
                Code(0x201c, state, link);
            else if (word == "rdblquote")
                Code(0x201d, state, link);
            else if (word == "bullet")
                Code(0x2022, state, link);
            else if (word == "super" || word == "sub" || word == "scaps" || word == "caps" ||
                     word == "outline" || word == "v")
                document.editable = false;
        }
        throw std::runtime_error("unclosed RTF group");
    }

  public:
    explicit Reader(std::map<std::string, std::string> files) {
        document.assets = std::move(files);
        source = document.assets.at("TXT.rtf");
    }
    RichDocument Read() {
        if (source.substr(0, 5) != "{\\rtf")
            throw std::runtime_error("invalid RTF header");
        Group({}, 0, nullptr);
        while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor])))
            ++cursor;
        if (cursor != source.size() || pending_high)
            throw std::runtime_error("trailing RTF bytes");
        document.assets.erase("TXT.rtf");
        (void)CharacterCount(document.PlainText());
        return std::move(document);
    }
};
std::string Escape(std::string_view text) {
    (void)CharacterCount(text);
    const auto chars = icu::UnicodeString::fromUTF8(icu::StringPiece(text.data(), text.size()));
    std::string out;
    for (int32_t i = 0; i < chars.length(); ++i) {
        auto code = chars.charAt(i);
        if (code == '\\' || code == '{' || code == '}') {
            out += '\\';
            out += static_cast<char>(code);
        } else if (code == '\n')
            out += "\\par ";
        else if (code == '\r')
            continue;
        else if (code == '\t')
            out += "\\tab ";
        else if (code >= 32 && code < 127)
            out += static_cast<char>(code);
        else
            out += "\\u" + std::to_string(static_cast<std::int16_t>(code)) + "?";
    }
    return out;
}
} // namespace
bool RichRun::SameStyle(const RichRun& other) const {
    return std::tie(bold, italic, underline, strike, link, asset, width_twips, height_twips) ==
           std::tie(other.bold, other.italic, other.underline, other.strike, other.link,
                    other.asset, other.width_twips, other.height_twips);
}
std::string RichDocument::PlainText() const {
    std::string out;
    for (const auto& run : runs)
        out += run.asset.empty() ? run.text : "\xef\xbf\xbc";
    return out;
}
std::string RichDocument::Markdown() const {
    std::string out;
    for (const auto& run : runs) {
        if (!run.asset.empty()) {
            out += "[内嵌图片保存在富文本资产中]";
            continue;
        }
        auto text = run.text;
        if (text.empty())
            continue;
        if (run.bold)
            text = "**" + text + "**";
        if (run.italic)
            text = "*" + text + "*";
        if (!run.link.empty())
            text = "[" + text + "](" + run.link + ")";
        out += text;
    }
    return out;
}
std::string Base64Encode(std::string_view bytes) {
    if (bytes.size() > limit)
        throw std::runtime_error("asset limit");
    std::string out(4 * ((bytes.size() + 2) / 3), '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                    reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    return out;
}
std::optional<std::string> Base64Decode(std::string_view text) {
    if (text.empty() || text.size() % 4 || text.size() > 4 * ((limit + 2) / 3))
        return {};
    std::string out(3 * (text.size() / 4), '\0');
    const int count =
        EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                        reinterpret_cast<const unsigned char*>(text.data()), text.size());
    if (count < 0)
        return {};
    std::size_t padding = 0;
    if (text.back() == '=')
        ++padding;
    if (text[text.size() - 2] == '=')
        ++padding;
    if (static_cast<std::size_t>(count) < padding)
        return {};
    out.resize(count - padding);
    if (Base64Encode(out) != text)
        return {};
    return out;
}
std::optional<RichDocument> ReadRichText(std::string_view bytes) {
    try {
        return Reader(Unpack(bytes)).Read();
    } catch (const std::exception&) {
        return {};
    }
}
std::optional<std::string> WriteRichText(const RichDocument& document) {
    try {
        if (!document.editable || document.assets.size() > 256 ||
            CharacterCount(document.PlainText()) > 256 * 1024)
            return {};
        std::string rtf = "{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl{"
                          "\\f0\\fmodern Monospace;}}\\f0\\fs24\\uc1 ";
        auto files = document.assets;
        for (const auto& [name, bytes] : files)
            if (!SafeName(name) || name == "TXT.rtf" || bytes.size() > limit)
                return {};
        for (const auto& run : document.runs) {
            if (!run.asset.empty()) {
                if (!SafeName(run.asset) || run.asset.find_first_of("{}\\") != run.asset.npos ||
                    !files.count(run.asset))
                    return {};
                if (run.width_twips < 0 || run.height_twips < 0 || run.width_twips > 1000000 ||
                    run.height_twips > 1000000)
                    return {};
                rtf += "{{\\NeXTGraphic " + run.asset + " \\width" +
                       std::to_string(run.width_twips) + " \\height" +
                       std::to_string(run.height_twips) + " }";
                rtf += char(0xac);
                rtf += '}';
                continue;
            }
            if (!run.link.empty()) {
                if (run.link.find_first_of("\"\r\n") != run.link.npos)
                    return {};
                rtf += "{\\field{\\*\\fldinst{HYPERLINK \"" + Escape(run.link) + "\"}}{\\fldrslt ";
            }
            rtf += "{\\b" + std::to_string(run.bold) + "\\i" + std::to_string(run.italic) + "\\ul" +
                   std::to_string(run.underline) + "\\strike" + std::to_string(run.strike) + " " +
                   Escape(run.text) + "}";
            if (!run.link.empty())
                rtf += "}}";
            if (rtf.size() > limit)
                return {};
        }
        rtf += '}';
        files.emplace("TXT.rtf", std::move(rtf));
        return Pack(std::move(files));
    } catch (const std::exception&) {
        return {};
    }
}
} // namespace rimes::capsule
