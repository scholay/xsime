#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace rimes::windows::ui {

// Theme definitions correspond to DesignSystem/src/design-system/tokens/themes.json.
// No runtime file dependency.
enum class ThemeId : std::uint8_t { kNight = 0, kDay, kQuiet, kRasta };

inline constexpr const char* kThemeIds[] = {"night", "day", "quiet", "rasta"};
inline constexpr const wchar_t* kThemeTitles[] = {L"墨竹", L"翡翠", L"静谧",
                                                  L"拉斯塔"};
inline constexpr const wchar_t* kThemeFamilies[] = {L"经典", L"经典", L"经典",
                                                    L"拉斯塔"};

inline constexpr const wchar_t* kThemeDetails[] = {
    L"经典深色配色，层级清晰，适合长时间输入。",
    L"经典浅色配色，柔和边界与固定产品绿。",
    L"经典去色配色，降低视觉刺激。",
    L"深色精致骨架，以红、黄、绿三色共同组织状态与操作。"};

struct ThemePalette {
  std::uint32_t brand_red;
  std::uint32_t brand_yellow;
  std::uint32_t brand_green;
  std::uint32_t accent;
  std::uint32_t accent_foreground;
  std::uint32_t accent_text;
  std::uint32_t settings_background;
  std::uint32_t settings_separator;
  std::uint32_t buffer;
  std::uint32_t buffer_secondary;
  std::uint32_t buffer_border;
  std::uint32_t buffer_divider;
  std::uint32_t buffer_source_rail;
  std::uint32_t buffer_target_rail;
  std::uint32_t buffer_chip;
  std::uint32_t buffer_chip_selected;
  std::uint32_t buffer_preedit;
  std::uint32_t buffer_muted;
  std::uint32_t clipboard_selected;
  std::uint32_t surface;
  std::uint32_t surface_secondary;
  std::uint32_t surface_tertiary;
  std::uint32_t border;
  std::uint32_t border_strong;
  std::uint32_t text_primary;
  std::uint32_t text_secondary;
  std::uint32_t text_muted;
  std::uint32_t selection;
  std::uint32_t selection_text;
  std::uint32_t candidate;
  std::uint32_t warning_text;
  std::uint32_t warning_surface;
  std::uint32_t warning_border;
  std::uint32_t danger_text;
  std::uint32_t danger_fill;
  std::uint32_t danger_foreground;
  std::uint32_t danger_border;
  bool dark;
};

inline constexpr ThemePalette kNightPalette{
    0xE34B3F, 0xF2C94C, 0x22C55E, 0x22C55E, 0x000000, 0x22C55E, 0x323232,
    0x464646, 0x0C1E33, 0x123458, 0x2C5A8C, 0x3A4C5D, 0x15191F, 0x122A21,
    0x143A27, 0x165030, 0x165030, 0x9AA2AE, 0x1A4430, 0x101318, 0x171B22,
    0x1E232C, 0x252A33, 0x607080, 0xF3F5F8, 0x9AA2AE, 0x838B98, 0x15803D,
    0xFFFFFF, 0x101318, 0xFF9230, 0x332923, 0x946D32, 0xFF4245, 0xA63A3A,
    0xFFFFFF, 0x8E2E2E, true};

inline constexpr ThemePalette kDayPalette{
    0xC73C32, 0xB77900, 0x16864A, 0x22C55E, 0x000000, 0x0F6A3F, 0xECECEC,
    0xD5D5D5, 0xF1F6FC, 0xE4EEF9, 0x8298B0, 0xB1B9C5, 0xF0F4F7, 0xE7F6EF,
    0xDAF3E6, 0xC5EDD6, 0xC9EED9, 0x4B5563, 0xCDEBDE, 0xF5F7FA, 0xEEF2F6,
    0xE7ECF2, 0xC9D2DE, 0x7C8797, 0x17202B, 0x334155, 0x4B5563, 0x0F6A3F,
    0xFFFFFF, 0xF8FAFC, 0x8A4B00, 0xFFF4E5, 0xA15C00, 0xB42318, 0xA63A3A,
    0xFFFFFF, 0x8E2E2E, false};

inline constexpr ThemePalette kQuietPalette{
    0xB5B5B5, 0xC8C8C8, 0xA3A3A3, 0xA3A3A3, 0x000000, 0xA3A3A3, 0x323232,
    0x464646, 0x111111, 0x1C1C1C, 0x6B6B6B, 0x474747, 0x191919, 0x272727,
    0x333333, 0x454545, 0x454545, 0xA3A3A3, 0x3C3C3C, 0x141414, 0x1B1B1B,
    0x252525, 0x3A3A3A, 0x737373, 0xF5F5F5, 0xC7C7C7, 0xA3A3A3, 0x6B6B6B,
    0xFFFFFF, 0x141414, 0xFF9230, 0x35291F, 0x946D32, 0xFF4245, 0xA63A3A,
    0xFFFFFF, 0x8E2E2E, true};

inline constexpr ThemePalette kRastaPalette{
    0xE95043, 0xF2C94C, 0x39C96B, 0x39C96B, 0x07110A, 0x6DDE8F, 0x1B1A15,
    0x45412F, 0x11120E, 0x202118, 0x746B3A, 0x4B4731, 0x211C12, 0x14251A,
    0x183923, 0x21532F, 0x4C3A16, 0xB7AE91, 0x2C3F24, 0x11110E, 0x1D1C16,
    0x28261B, 0x3B382A, 0x81765A, 0xFFF8E1, 0xD8CFB5, 0xADA58E, 0x287B42,
    0xFFFFFF, 0x11110E, 0xF7D36A, 0x352C16, 0x9B7B24, 0xFF786E, 0x9D332D,
    0xFFFFFF, 0xC84B42, true};

inline constexpr const ThemePalette* kThemePalettes[] = {
    &kNightPalette, &kDayPalette, &kQuietPalette, &kRastaPalette};

[[nodiscard]] inline constexpr ThemeId DefaultThemeId() noexcept {
  return ThemeId::kNight;
}

[[nodiscard]] inline constexpr const ThemePalette& Palette(
    ThemeId id) noexcept {
  const auto index = static_cast<std::size_t>(id);
  if (index >= 4) {
    return kNightPalette;
  }
  return *kThemePalettes[index];
}

[[nodiscard]] inline constexpr const char* ThemeIdString(ThemeId id) noexcept {
  const auto index = static_cast<std::size_t>(id);
  return index < 4 ? kThemeIds[index] : kThemeIds[0];
}

[[nodiscard]] inline std::optional<ThemeId> ParseThemeId(
    std::string_view value) noexcept {
  for (std::size_t i = 0; i < 4; ++i) {
    if (value == kThemeIds[i]) {
      return static_cast<ThemeId>(i);
    }
  }
  return std::nullopt;
}

// Unknown values fall back to night without failing callers that only want a
// display palette. Validation for settings persistence is separate.
[[nodiscard]] inline ThemeId ThemeIdOrDefault(std::string_view value) noexcept {
  return ParseThemeId(value).value_or(DefaultThemeId());
}

[[nodiscard]] inline constexpr std::uint8_t Red(std::uint32_t rgb) noexcept {
  return static_cast<std::uint8_t>((rgb >> 16) & 0xff);
}
[[nodiscard]] inline constexpr std::uint8_t Green(std::uint32_t rgb) noexcept {
  return static_cast<std::uint8_t>((rgb >> 8) & 0xff);
}
[[nodiscard]] inline constexpr std::uint8_t Blue(std::uint32_t rgb) noexcept {
  return static_cast<std::uint8_t>(rgb & 0xff);
}

[[nodiscard]] inline constexpr float Rf(std::uint32_t rgb) noexcept {
  return static_cast<float>(Red(rgb)) / 255.0f;
}
[[nodiscard]] inline constexpr float Gf(std::uint32_t rgb) noexcept {
  return static_cast<float>(Green(rgb)) / 255.0f;
}
[[nodiscard]] inline constexpr float Bf(std::uint32_t rgb) noexcept {
  return static_cast<float>(Blue(rgb)) / 255.0f;
}

[[nodiscard]] inline constexpr std::uint32_t Blend(std::uint32_t a,
                                                   std::uint32_t b,
                                                   float amount) noexcept {
  const float t = amount < 0.f ? 0.f : (amount > 1.f ? 1.f : amount);
  const auto mix = [t](std::uint8_t x, std::uint8_t y) {
    return static_cast<std::uint8_t>(
        static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t);
  };
  return (static_cast<std::uint32_t>(mix(Red(a), Red(b))) << 16) |
         (static_cast<std::uint32_t>(mix(Green(a), Green(b))) << 8) |
         static_cast<std::uint32_t>(mix(Blue(a), Blue(b)));
}

}  // namespace rimes::windows::ui
