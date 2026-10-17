#pragma once
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace rimes::capsule {
struct RichRun {
    std::string text;
    bool bold = false, italic = false, underline = false, strike = false;
    std::string link, asset;
    int width_twips = 2000, height_twips = 2000;
    bool SameStyle(const RichRun& other) const;
};
struct RichDocument {
    std::vector<RichRun> runs;
    std::map<std::string, std::string> assets;
    bool editable = true;
    std::string PlainText() const;
    std::string Markdown() const;
};
std::string Base64Encode(std::string_view bytes);
std::optional<std::string> Base64Decode(std::string_view text);
// In-memory flat RTFD: names are never interpreted as filesystem paths.
std::optional<RichDocument> ReadRichText(std::string_view bytes);
std::optional<std::string> WriteRichText(const RichDocument& document);
} // namespace rimes::capsule
