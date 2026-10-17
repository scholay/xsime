#pragma once
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace rimes::capsule {
struct DetailRow {
    std::string type, label, value;
    bool secret = false;
};
struct DetailSection {
    std::string heading;
    std::vector<DetailRow> rows;
};
struct HeaderEntry {
    std::string key;
    std::vector<std::string> lines;
    std::optional<std::string> scalar;
};
struct FrontMatter {
    std::vector<HeaderEntry> entries;
    std::optional<std::string> Scalar(std::string_view key) const;
    std::vector<std::string> Preserved() const;
    std::vector<DetailRow> Fields() const;
};
std::optional<std::string> DecodeScalar(std::string_view text);
std::optional<FrontMatter> ParseFrontMatter(const std::vector<std::string>& lines);
std::vector<DetailSection> NoteSections(std::string_view body,
                                        const std::vector<DetailRow>& headers = {});
std::vector<DetailRow> FlattenSections(const std::vector<DetailSection>& sections);
bool SecretLabel(std::string_view label);
std::string NoteSummary(std::string_view body);
std::string NormalizeSummary(std::string_view summary);
std::optional<std::string> ToggleBullet(std::string_view line);
std::string IndentBullet(std::string_view line, bool outdent);
} // namespace rimes::capsule
