#include "capsule_reference.hpp"
#include "capsule_store.hpp"
#include "capsule_text.hpp"
#include <algorithm>
namespace rimes::capsule {
std::string Reference::Get(const std::string& key) const {
    const auto found = fields.find(key);
    return found == fields.end() ? "" : found->second;
}
std::vector<std::string> Reference::Missing() const {
    std::vector<std::string> missing;
    if (TrimText(Get("authors")).empty())
        missing.push_back("作者");
    if (TrimText(Get("year")).empty())
        missing.push_back("年份");
    if (kind == "book" || kind == "report") {
        if (TrimText(Get("publisher")).empty())
            missing.push_back("出版社 / 机构");
    } else if (TrimText(Get("container")).empty())
        missing.push_back("期刊 / 会议");
    return missing;
}
std::string Reference::SearchText() const {
    std::string text;
    for (const auto* key : kReferenceFields)
        text += "\n" + Get(key);
    return text;
}
bool ReferenceKey(std::string_view key) {
    if (key == "reference_type")
        return true;
    for (const auto* field : kReferenceFields)
        if (key == std::string("reference_") + field)
            return true;
    return false;
}
Reference ReadReference(const FrontMatter& header) {
    Reference value;
    const auto kind = header.Scalar("reference_type").value_or("");
    if (kind == "book" || kind == "journalArticle" || kind == "conferencePaper" || kind == "report")
        value.kind = kind;
    for (const auto* key : kReferenceFields)
        value.fields[key] = header.Scalar(std::string("reference_") + key).value_or("");
    return value;
}
std::vector<std::string> ReferenceHeader(const Reference& value) {
    std::vector<std::string> result = {"reference_type: " + JsonQuote(value.kind)};
    for (const auto* key : kReferenceFields)
        if (!TrimText(value.Get(key)).empty())
            result.push_back(std::string("reference_") + key + ": " + JsonQuote(value.Get(key)));
    return result;
}
std::optional<std::string> FormatReference(std::string title, const Reference& value,
                                           std::string_view style) {
    title = TrimText(title);
    if (!value.Missing().empty() || title.empty())
        return {};
    std::string authors;
    const auto names = value.Get("authors");
    for (std::size_t start = 0; start <= names.size();) {
        auto end = names.find(';', start);
        if (end == names.npos)
            end = names.size();
        const auto name = TrimText(names.substr(start, end - start));
        if (!name.empty()) {
            if (!authors.empty())
                authors += ", ";
            authors += name;
        }
        if (end == names.size())
            break;
        start = end + 1;
    }
    if (authors.empty())
        return {};
    const auto year = TrimText(value.Get("year"));
    const auto source =
        value.Get(value.kind == "book" || value.kind == "report" ? "publisher" : "container");
    const auto issue = value.Get("issue"), pages = value.Get("pages");
    const auto detail = value.Get("volume") + (issue.empty() ? "" : "(" + issue + ")") +
                        (pages.empty() ? "" : ": " + pages);
    auto locator = value.Get("doi");
    if (!locator.empty()) {
        constexpr std::string_view prefix = "https://doi.org/";
        for (auto at = locator.find(prefix); at != locator.npos; at = locator.find(prefix, at))
            locator.erase(at, prefix.size());
        locator = std::string(prefix) + locator;
    } else
        locator = value.Get("url");
    const auto suffix = locator.empty() ? "" : " " + locator;
    const auto comma = detail.empty() ? "" : ", " + detail;
    if (style == "gbt7714") {
        const auto marker = value.kind == "book"              ? "M"
                            : value.kind == "journalArticle"  ? "J"
                            : value.kind == "conferencePaper" ? "C"
                                                              : "R";
        return authors + ". " + title + "[" + marker + "]. " + source + ", " + year + comma + "." +
               suffix;
    }
    if (style == "apa7")
        return authors + ". (" + year + "). " + title + ". " + source + comma + "." + suffix;
    if (style == "mla9")
        return authors + ". \"" + title + ".\" " + source + ", " + year + comma + "." + suffix;
    if (style == "chicago-author-date")
        return authors + ". " + year + ". \"" + title + ".\" " + source + comma + "." + suffix;
    if (style == "vancouver")
        return authors + ". " + title + ". " + source + ". " + year +
               (detail.empty() ? "" : ";" + detail) + "." + suffix;
    if (style == "ieee")
        return authors + ", \"" + title + ",\" " + source + ", " + year + comma + "." + suffix;
    return {};
}
} // namespace rimes::capsule
