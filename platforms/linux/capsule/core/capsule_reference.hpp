#pragma once
#include "capsule_structure.hpp"
#include <array>
#include <map>
#include <optional>
#include <string>
namespace rimes::capsule {
inline constexpr std::array<const char*, 10> kReferenceFields = {
    "authors", "year", "container", "publisher", "volume", "issue", "pages", "doi", "url", "isbn"};
inline constexpr std::array<const char*, 6> kCitationStyles = {
    "gbt7714", "apa7", "mla9", "chicago-author-date", "vancouver", "ieee"};
struct Reference {
    std::string kind = "book";
    std::map<std::string, std::string> fields;
    std::string Get(const std::string& key) const;
    std::vector<std::string> Missing() const;
    std::string SearchText() const;
};
Reference ReadReference(const FrontMatter& header);
bool ReferenceKey(std::string_view key);
std::vector<std::string> ReferenceHeader(const Reference& value);
std::optional<std::string> FormatReference(std::string title, const Reference& value,
                                           std::string_view style);
} // namespace rimes::capsule
