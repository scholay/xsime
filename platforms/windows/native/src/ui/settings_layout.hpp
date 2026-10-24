#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "candidate_strip.hpp"
#include "icons.hpp"
#include "fluent_icons.hpp"
#include "theme.hpp"
#include "../core/product_brand.hpp"

namespace rimes::windows::ui {

enum class SettingsPage {
  kInput = 0,
  kAppearance = 1,
  kBuffer = 2,
  kApi = 3,
  kPlugins = 4,
  kAbout = 5,
  kChord = 6,
  kCapsule = 7,
  kMailbox = 8,
  kTyping = 9,
  kStatistics = 10,
  kHome = 11,
};

inline constexpr int kSettingsPageCount = 12;
// Stable page identities are independent of sidebar order. Core hosts are
// peers; only plugin-contributed input features belong in Extensions.
inline constexpr std::array<SettingsPage, kSettingsPageCount> kSettingsNavOrder = {
    SettingsPage::kHome, SettingsPage::kInput, SettingsPage::kAppearance, SettingsPage::kBuffer,
    SettingsPage::kMailbox, SettingsPage::kCapsule, SettingsPage::kApi,
    SettingsPage::kPlugins, SettingsPage::kAbout, SettingsPage::kChord,
    SettingsPage::kTyping, SettingsPage::kStatistics};
inline int SettingsNavIndex(SettingsPage page) {
  return static_cast<int>(std::find(kSettingsNavOrder.begin(),
      kSettingsNavOrder.end(), page) - kSettingsNavOrder.begin());
}
inline const wchar_t* const kSettingsPageTitles[] = {
    L"输入法", L"外观", L"Buffer", L"连接器", L"插件", L"维护", L"并击", L"Capsule", L"Mailbox", L"打字测速", L"统计", rimes::branding::Name()};
inline constexpr FluentIcon kSettingsPageIcons[] = {
    FluentIcon::kKeyboard, FluentIcon::kPalette, FluentIcon::kBuffer, FluentIcon::kConnector,
    FluentIcon::kPlugin, FluentIcon::kMaintenance, FluentIcon::kChord, FluentIcon::kCapsule,
    FluentIcon::kMailbox, FluentIcon::kTyping, FluentIcon::kStatistics, FluentIcon::kHome};
inline constexpr const wchar_t* kSettingsSchemeTitles[] = {
    L"雾凇全拼", L"自然码双拼", L"小鹤双拼", L"五笔 86", L"犀术五笔", L"英文", L"isaac2026"};
inline constexpr FluentIcon kSettingsSchemeIcons[] = {
    FluentIcon::kText, FluentIcon::kKeyboard, FluentIcon::kKeyboard, FluentIcon::kGrid,
    FluentIcon::kGrid, FluentIcon::kLanguage, FluentIcon::kChord};
inline constexpr int kSettingsSchemeCount = sizeof(kSettingsSchemeTitles) / sizeof(kSettingsSchemeTitles[0]);
inline const wchar_t* const kSettingsSubpageLabels[][6] = {
    {L"输入方案", L"选项"},
    {L"主题", L"尺寸"},
    {L"快捷键", L"行为", L"窗口与全局快捷键"},
    {L"AI 模型", L"本地网关"},
    {L"全部", L"缓冲插件", L"Capsule 插件", L"Mailbox 插件", L"内置扩展"},
    {L"版本", L"诊断"},
    {L"isaac2026", L"键位与预览"},
    {L"Capsule"},
    {L"工作区", L"说明"},
    {L"文章跟打", L"成绩"},
    {L"每日", L"历史", L"测速成绩"},
    {rimes::branding::Name(), L"官网", L"邮箱", L"开源项目", L"README", L"License"},
};
inline int SettingsTabCount(SettingsPage page) { return page == SettingsPage::kCapsule ? 1 : page == SettingsPage::kHome ? 6 : page == SettingsPage::kPlugins ? 5 : page == SettingsPage::kStatistics || page == SettingsPage::kBuffer ? 3 : 2; }

struct SettingsHomeLink {
  const wchar_t* title;
  const wchar_t* summary;
  const wchar_t* action;
  const wchar_t* destination;
};
inline constexpr SettingsHomeLink kSettingsHomeLinks[] = {
    {L"xsime.com", L"访问 {brand} 官网，了解产品与项目动态。", L"打开官网", L"https://xsime.com"},
    {L"pm@scholay.com", L"反馈使用体验、问题或合作想法。", L"写邮件", L"mailto:pm@scholay.com"},
    {L"scholay / xsime", L"查看源码、提交问题或参与改进。", L"查看源码", L"https://github.com/scholay/xsime"},
    {L"从这里了解 {brand}", L"输入法、Buffer、Capsule 与 Mailbox 的功能和安装说明。", L"阅读 README", L"https://github.com/scholay/xsime/blob/main/README.md"},
    {L"Apache License 2.0", L"查看 {brand} 的开源许可原文。", L"查看完整许可", L"https://github.com/scholay/xsime/blob/main/LICENSE"},
};
inline const SettingsHomeLink* HomeLink(int subpage) {
  return subpage >= 1 && subpage <= 5 ? &kSettingsHomeLinks[subpage - 1] : nullptr;
}

struct SettingsMetrics {
  int client_width_dip = 980;
  int client_height_dip = 680;
  int min_client_width_dip = 860;
  int min_client_height_dip = 600;
  int sidebar_width_dip = 160;
  int sidebar_pad_x_dip = 12;
  int sidebar_header_y_dip = 16;
  int sidebar_row_y_dip = 36;
  int sidebar_row_height_dip = 32;
  int sidebar_row_gap_dip = 4;
  int sidebar_icon_dip = 18;
  int sidebar_font_dip = 12;
  int subpage_bar_height_dip = 46;
  int heading_band_height_dip = 84;
  int body_origin_x_dip = 184;
  int body_origin_y_dip = 130;
  int body_max_width_dip = 650;
  int choice_card_height_dip = 68;
  int choice_gap_dip = 8;
  int theme_card_height_dip = 116;
  int theme_preview_height_dip = 68;
  int theme_swatch_height_dip = 30;
  int control_radius_dip = 8;
  int card_radius_dip = 11;
};

struct SettingsDraft {
  std::array<bool, 3> extensions_enabled{true, true, true};
  ThemeId theme = ThemeId::kNight;
  ThemeId preview_theme = ThemeId::kNight;
  SettingsPage page = SettingsPage::kHome;
  int subpage = 0;
  int schema_index = 0;
  std::wstring imported_schema;
  int theme_index = 0;
  int theme_detail = -1;
  // Keyboard focus: -1 none, 0..4 sidebar, 100+ scheme, 200+ theme.
  int focus = -1;
  bool keyboard_focus = false;
  int hover = -1;
  bool ascii = false;
  bool traditional = false;
  bool ascii_punctuation = false;
  unsigned font_size = 16;
  unsigned candidate_count = 9;
  bool vertical_candidates = false;
  bool close_after_send = false;
  bool align_to_input_box = true, reset_on_app_switch = false;
  unsigned auto_send_seconds = 0;
  int translation_provider_index = 0;
  unsigned hotkey_modifiers = MOD_ALT | MOD_SHIFT;
  wchar_t hotkey = L'B';
  unsigned send_modifiers = MOD_ALT | MOD_SHIFT, send_key = VK_RETURN;
  std::array<unsigned, 2> plugin_keys{VK_UP, VK_DOWN}, plugin_modifiers{MOD_ALT | MOD_SHIFT, MOD_ALT | MOD_SHIFT};
  std::array<unsigned, 3> global_keys{'S', 'V', '4'}, global_modifiers{MOD_ALT | MOD_SHIFT, MOD_ALT | MOD_SHIFT, MOD_ALT | MOD_SHIFT};
  std::wstring base_url;
  std::wstring model;
  int ai_connector_index = 0;
  std::wstring codex_path, codex_model;
  std::wstring target_language = L"English";
  std::wstring source_language = L"Chinese";
  std::wstring api_key;  // fixture / draft only; never loaded from secrets
  std::wstring about_text;
  std::wstring save_error;
};

inline bool SettingsPageAvailable(const SettingsDraft& draft, SettingsPage page) {
  return page == SettingsPage::kChord ? draft.extensions_enabled[0] :
      page == SettingsPage::kTyping ? draft.extensions_enabled[1] :
      page == SettingsPage::kStatistics ? draft.extensions_enabled[2] : true;
}

struct SettingsLayout {
  float width_dip = 980;
  float height_dip = 680;
  DipRect sidebar{};
  DipRect divider{};
  DipRect content{};
  DipRect subpage_bar{};
  DipRect heading{};
  DipRect body{};
  DipRect status_bar{};
  std::array<DipRect, kSettingsPageCount> nav{};
  std::vector<DipRect> scheme_cards;
  std::vector<DipRect> theme_cards;
  std::vector<DipRect> theme_details;
  DipRect theme_popover{};
  std::array<DipRect, 6> subpage_tabs{};
  DipRect home_hero{}, home_action{};
  DipRect font_edit{};
  DipRect candidate_count_edit{};
  DipRect check_vertical{};
  DipRect hotkey_modifiers{};
  DipRect hotkey_edit{};
  DipRect send_modifiers{}, send_edit{}, hotkey_record{}, send_record{}, shortcut_reset{}, shortcut_feedback{};
  std::array<DipRect, 2> plugin_modifiers{}, plugin_edit{}, plugin_record{};
  std::array<DipRect, 3> global_modifiers{}, global_edit{}, global_record{};
  DipRect base_edit{};
  DipRect connector_combo{}, codex_path_edit{}, codex_model_edit{};
  DipRect model_edit{};
  DipRect key_edit{};
  DipRect lang_edit{};
  DipRect check_ascii{};
  DipRect check_trad{};
  DipRect check_punct{};
  DipRect auto_send_combo{}, close_after_check{}, translation_combo{}, buffer_language{};
  DipRect buffer_source_language{};
};

[[nodiscard]] inline float SettingsBodyWidth(float client_width_dip,
                                             const SettingsMetrics& m = {}) {
  const float available =
      client_width_dip - static_cast<float>(m.body_origin_x_dip) - 24.0f;
  return (std::clamp)(available, 320.0f,
                      static_cast<float>(m.body_max_width_dip));
}

[[nodiscard]] inline SettingsLayout LayoutSettings(
    float width_dip, float height_dip, const SettingsDraft& draft,
    const SettingsMetrics& m = {}) {
  SettingsLayout layout;
  width_dip = (std::max)(width_dip, static_cast<float>(m.min_client_width_dip));
  height_dip =
      (std::max)(height_dip, static_cast<float>(m.min_client_height_dip));
  layout.width_dip = width_dip;
  layout.height_dip = height_dip;

  const float side_w = static_cast<float>(m.sidebar_width_dip);
  layout.sidebar = {0, 0, side_w, height_dip};
  layout.divider = {side_w, 0, side_w + 1.0f, height_dip};
  layout.content = {side_w + 1.0f, 0, width_dip, height_dip};
  layout.subpage_bar = {layout.content.left, 0, layout.content.right,
                        static_cast<float>(m.subpage_bar_height_dip)};
  layout.heading = {layout.content.left, layout.subpage_bar.bottom,
                    layout.content.right,
                    layout.subpage_bar.bottom +
                        static_cast<float>(m.heading_band_height_dip)};
  const float body_w = SettingsBodyWidth(width_dip, m);
  layout.body = {static_cast<float>(m.body_origin_x_dip),
                 static_cast<float>(m.body_origin_y_dip),
                 static_cast<float>(m.body_origin_x_dip) + body_w,
                 height_dip - 16.0f};

  const float pad = static_cast<float>(m.sidebar_pad_x_dip);
  float ny = static_cast<float>(m.sidebar_row_y_dip);
  for (const auto page : kSettingsNavOrder) {
    const auto i = static_cast<int>(page);
    if (page == SettingsPage::kChord) ny += 28;
    if (!SettingsPageAvailable(draft, page)) continue;
    layout.nav[static_cast<std::size_t>(i)] = {
        pad, ny, side_w - pad,
        ny + static_cast<float>(m.sidebar_row_height_dip)};
    ny += static_cast<float>(m.sidebar_row_height_dip + m.sidebar_row_gap_dip);
  }

  float tab_x = layout.content.left + 24.0f;
  for (int i = 0; i < SettingsTabCount(draft.page); ++i) {
    float text_width = 0;
    for (const wchar_t* c = kSettingsSubpageLabels[static_cast<int>(draft.page)][i]; *c; ++c)
      text_width += *c == L' ' ? 3.5f : *c < 128 ? 6.5f : 11.0f;
    const float tab_width = (std::max)(44.0f, 20.0f + text_width);
    layout.subpage_tabs[static_cast<std::size_t>(i)] = {
        tab_x, 12, tab_x + tab_width, 34};
    tab_x += tab_width;
  }
  layout.status_bar = {tab_x + 16, 8, layout.content.right - 12, 38};

  const float gap = static_cast<float>(m.choice_gap_dip);
  if (draft.page == SettingsPage::kHome) {
    layout.home_hero = {layout.body.left, layout.body.top, layout.body.right, layout.body.top + 168};
    if (draft.subpage == 0) layout.home_action = {layout.body.left, layout.body.top + 220, layout.body.left + 160, layout.body.top + 254};
    if (HomeLink(draft.subpage))
      layout.home_action = {layout.body.left, layout.body.top + 106, layout.body.left + 160, layout.body.top + 140};
  }
  if (draft.page == SettingsPage::kInput && draft.subpage == 0 && draft.imported_schema.empty()) {
    const int columns = 3;
    const float card_h = static_cast<float>(m.choice_card_height_dip);
    const float card_w = (body_w - gap * (columns - 1)) / columns;
    layout.scheme_cards.resize(kSettingsSchemeCount);
    for (int i = 0; i < kSettingsSchemeCount; ++i) {
      const int row = i / columns;
      const int col = i % columns;
      const float x = layout.body.left + static_cast<float>(col) * (card_w + gap);
      const float y = layout.body.top + static_cast<float>(row) * (card_h + gap);
      layout.scheme_cards[static_cast<std::size_t>(i)] = {x, y, x + card_w,
                                                          y + card_h};
    }
  } else if (draft.page == SettingsPage::kInput) {
    float cy = layout.body.top;
    layout.check_ascii = {layout.body.left, cy, layout.body.left + 220.0f,
                          cy + 24.0f};
    cy += 28.0f;
    layout.check_trad = {layout.body.left, cy, layout.body.left + 220.0f,
                         cy + 24.0f};
    cy += 28.0f;
    layout.check_punct = {layout.body.left, cy, layout.body.left + 220.0f,
                          cy + 24.0f};
  } else if (draft.page == SettingsPage::kAppearance && draft.subpage == 0) {
    const int columns = 2;
    const float card_h = static_cast<float>(m.theme_card_height_dip);
    const float card_w = (body_w - gap) / 2.0f;
    layout.theme_cards.resize(4);
    layout.theme_details.resize(4);
    for (int i = 0; i < 4; ++i) {
      const int row = i / columns;
      const int col = i % columns;
      const float x = layout.body.left + static_cast<float>(col) * (card_w + gap);
      const float y = layout.body.top + 24.0f +
                      static_cast<float>(row) * (card_h + gap);
      layout.theme_cards[static_cast<std::size_t>(i)] = {x, y, x + card_w,
                                                         y + card_h};
      layout.theme_details[static_cast<std::size_t>(i)] = {
          x + card_w - 31, y + card_h - 30, x + card_w - 9, y + card_h - 8};
    }
  } else if (draft.page == SettingsPage::kAppearance) {
    const float fy = layout.body.top;
    layout.font_edit = {layout.body.left + 232.0f, fy, layout.body.left + 328.0f,
                        fy + 29.0f};
    layout.candidate_count_edit = {layout.font_edit.left, fy + 44.f, layout.font_edit.right, fy + 73.f};
    layout.check_vertical = {layout.body.left + 16.f, fy + 88.f, layout.body.right, fy + 118.f};
  } else if (draft.page == SettingsPage::kBuffer && draft.subpage == 1) {
    const float x = layout.body.left + 200, y = layout.body.top;
    layout.auto_send_combo = {x, y + 22, layout.body.right - 18, y + 52};
    layout.close_after_check = {layout.body.left + 18, y + 78, layout.body.right - 18, y + 108};
    layout.translation_combo = {x, y + 172, layout.body.right - 18, y + 202};
    layout.buffer_source_language = {x, y + 216, layout.body.right - 18, y + 246};
    layout.buffer_language = {x, y + 260, layout.body.right - 18, y + 290};
  } else if (draft.page == SettingsPage::kBuffer && draft.subpage == 0) {
    layout.hotkey_modifiers = {layout.body.left, layout.body.top + 28.0f,
                              layout.body.left + 130.0f, layout.body.top + 54.0f};
    layout.hotkey_edit = {layout.body.left + 172.0f, layout.body.top + 28.0f,
                          layout.body.left + 280.0f, layout.body.top + 54.0f};
    layout.hotkey_modifiers.right = layout.body.left + 160.0f;
    layout.send_modifiers = {layout.hotkey_modifiers.left, layout.body.top + 92.0f,
                             layout.hotkey_modifiers.right, layout.body.top + 118.0f};
    layout.send_edit = {layout.hotkey_edit.left, layout.send_modifiers.top,
                        layout.hotkey_edit.right, layout.send_modifiers.bottom};
    layout.hotkey_record = {layout.body.left + 296, layout.hotkey_edit.top,
                            layout.body.left + 414, layout.hotkey_edit.bottom};
    layout.send_record = {layout.hotkey_record.left, layout.send_edit.top,
                          layout.hotkey_record.right, layout.send_edit.bottom};
    for (int i = 0; i < 2; ++i) {
      const float y = layout.body.top + 156 + i * 64.f;
      layout.plugin_modifiers[i] = {layout.hotkey_modifiers.left, y, layout.hotkey_modifiers.right, y + 26};
      layout.plugin_edit[i] = {layout.hotkey_edit.left, y, layout.hotkey_edit.right, y + 26};
      layout.plugin_record[i] = {layout.hotkey_record.left, y, layout.hotkey_record.right, y + 26};
    }
    layout.shortcut_reset = {layout.body.left, layout.body.top + 312,
                             layout.body.left + 170, layout.body.top + 340};
    layout.shortcut_feedback = {layout.body.left, layout.body.top + 350,
                                layout.body.right, layout.body.top + 422};
  }
  if (draft.page == SettingsPage::kBuffer && draft.subpage == 2) {
    for (int i = 0; i < 3; ++i) {
      const float y = layout.body.top + 26 + i * 64.f;
      layout.global_modifiers[i] = {layout.body.left, y, layout.body.left + 190, y + 26};
      layout.global_edit[i] = {layout.body.left + 200, y, layout.body.left + 280, y + 26};
      layout.global_record[i] = {layout.body.left + 296, y, layout.body.left + 414, y + 26};
    }
    layout.shortcut_reset = {layout.body.left, layout.body.top + 326, layout.body.left + 190, layout.body.top + 354};
    layout.shortcut_feedback = {layout.body.left, layout.body.top + 364, layout.body.right, layout.body.top + 432};
  }
  if (draft.page == SettingsPage::kAppearance && draft.subpage == 0 &&
      draft.theme_detail >= 0 && draft.theme_detail < 4) {
    const auto& anchor = layout.theme_details[static_cast<std::size_t>(draft.theme_detail)];
    const float left = (std::clamp)(anchor.right - 260.0f,
                                    layout.content.left + 12.0f, width_dip - 272.0f);
    const float top = (std::min)(anchor.bottom + 8.0f, layout.body.bottom - 148.0f);
    layout.theme_popover = {left, top, left + 260.0f, top + 136.0f};
  }
  return layout;
}

// The details button owns its region before its parent theme card. This also
// supplies one stable mouse-down/up identity to the production settings host.
[[nodiscard]] inline int HitTestSettings(const SettingsLayout& layout,
                                          float x, float y) noexcept {
  const auto hit = [&](const DipRect& r) {
    return r.width() > 0 && r.height() > 0 && r.contains(x, y);
  };
  if (hit(layout.theme_popover)) return 600;
  for (std::size_t i = 0; i < layout.theme_details.size(); ++i)
    if (hit(layout.theme_details[i])) return 400 + static_cast<int>(i);
  for (std::size_t i = 0; i < layout.nav.size(); ++i)
    if (hit(layout.nav[i])) return static_cast<int>(i);
  for (std::size_t i = 0; i < layout.subpage_tabs.size(); ++i)
    if (hit(layout.subpage_tabs[i])) return 500 + static_cast<int>(i);
  for (std::size_t i = 0; i < layout.scheme_cards.size(); ++i)
    if (hit(layout.scheme_cards[i])) return 100 + static_cast<int>(i);
  for (std::size_t i = 0; i < layout.theme_cards.size(); ++i)
    if (hit(layout.theme_cards[i])) return 200 + static_cast<int>(i);
  return -1;
}

}  // namespace rimes::windows::ui
