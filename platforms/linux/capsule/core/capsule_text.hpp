#pragma once
#include <cstddef>
#include <string>
#include <string_view>
namespace rimes::capsule {
std::string SalvageText(std::string_view text, std::size_t count);
// Native Swift String limits count extended grapheme clusters, not UTF-8 bytes.
std::size_t CharacterCount(std::string_view text);
std::string CharacterPrefix(std::string_view text, std::size_t count);
std::string TrimText(std::string_view text);
std::string LowerText(std::string_view text);
std::string OneLine(std::string_view text);
} // namespace rimes::capsule
