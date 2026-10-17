#include "capsule_text.hpp"
#include <memory>
#include <stdexcept>
#include <unicode/brkiter.h>
#include <unicode/uchar.h>
#include <unicode/unistr.h>
#include <unicode/ustring.h>
#include <vector>
namespace rimes::capsule {
namespace {
icu::UnicodeString Decode(std::string_view text) {
    if (text.size() > 8 * 1024 * 1024 || text.find('\0') != text.npos)
        throw std::runtime_error("invalid Capsule text");
    UErrorCode status = U_ZERO_ERROR;
    int32_t size = 0;
    u_strFromUTF8(nullptr, 0, &size, text.data(), static_cast<int32_t>(text.size()), &status);
    if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status))
        throw std::runtime_error("invalid UTF-8");
    status = U_ZERO_ERROR;
    std::vector<UChar> chars(size + 1);
    u_strFromUTF8(chars.data(), chars.size(), &size, text.data(), text.size(), &status);
    if (U_FAILURE(status))
        throw std::runtime_error("invalid UTF-8");
    return icu::UnicodeString(chars.data(), size);
}
std::string Encode(const icu::UnicodeString& text) {
    std::string result;
    text.toUTF8String(result);
    return result;
}
std::unique_ptr<icu::BreakIterator> Breaks(const icu::UnicodeString& text) {
    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::BreakIterator> result(
        icu::BreakIterator::createCharacterInstance(icu::Locale::getRoot(), status));
    if (U_FAILURE(status) || !result)
        throw std::runtime_error("Unicode rules unavailable");
    result->setText(text);
    return result;
}
} // namespace
std::string SalvageText(std::string_view text, std::size_t count) {
    if (text.size() > 8 * 1024 * 1024)
        throw std::runtime_error("invalid Capsule text");
    auto decoded = icu::UnicodeString::fromUTF8(icu::StringPiece(text.data(), text.size()));
    decoded.findAndReplace(icu::UnicodeString(UChar(0)), icu::UnicodeString());
    auto iterator = Breaks(decoded);
    int32_t end = 0;
    for (iterator->first(); count > 0; --count) {
        const auto next = iterator->next();
        if (next == icu::BreakIterator::DONE)
            break;
        end = next;
    }
    return Encode(decoded.tempSubStringBetween(0, end));
}
std::size_t CharacterCount(std::string_view text) {
    const auto decoded = Decode(text);
    auto iterator = Breaks(decoded);
    std::size_t count = 0;
    for (iterator->first(); iterator->next() != icu::BreakIterator::DONE;)
        ++count;
    return count;
}
std::string CharacterPrefix(std::string_view text, std::size_t count) {
    const auto decoded = Decode(text);
    auto iterator = Breaks(decoded);
    int32_t end = 0;
    for (iterator->first(); count > 0; --count) {
        const auto next = iterator->next();
        if (next == icu::BreakIterator::DONE)
            break;
        end = next;
    }
    return Encode(decoded.tempSubStringBetween(0, end));
}
std::string TrimText(std::string_view text) {
    auto decoded = Decode(text);
    int32_t start = 0, end = decoded.length();
    while (start < end && u_isUWhiteSpace(decoded.char32At(start)))
        start = decoded.moveIndex32(start, 1);
    while (end > start) {
        const auto previous = decoded.moveIndex32(end, -1);
        if (!u_isUWhiteSpace(decoded.char32At(previous)))
            break;
        end = previous;
    }
    return Encode(decoded.tempSubStringBetween(start, end));
}
std::string LowerText(std::string_view text) {
    auto decoded = Decode(text);
    decoded.toLower(icu::Locale::getRoot());
    return Encode(decoded);
}
std::string OneLine(std::string_view text) {
    const auto decoded = Decode(text);
    icu::UnicodeString result;
    bool space = false;
    for (int32_t index = 0; index < decoded.length();) {
        const auto code = decoded.char32At(index);
        index = decoded.moveIndex32(index, 1);
        if (u_isUWhiteSpace(code)) {
            space = !result.isEmpty();
            continue;
        }
        if (space)
            result.append(UChar32(' '));
        space = false;
        result.append(code);
    }
    return Encode(result);
}
} // namespace rimes::capsule
