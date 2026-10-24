#include "../core/product_brand.hpp"
#pragma once

#include <Windows.h>

#include <string>
#include "rimes_version.hpp"

#include "icons.hpp"
#include "settings_layout.hpp"
#include "theme.hpp"

namespace rimes::windows::ui {

struct SettingsFonts {
  HFONT sidebar = nullptr;
  HFONT title = nullptr;
  HFONT heading = nullptr;
  HFONT body = nullptr;
  HFONT family = nullptr;
  HFONT hint = nullptr;
  HFONT control = nullptr;
  HFONT brand = nullptr;
  unsigned dpi = 96;

  void Release() noexcept {
    if (sidebar) DeleteObject(sidebar);
    if (title) DeleteObject(title);
    if (heading) DeleteObject(heading);
    if (body) DeleteObject(body);
    if (family) DeleteObject(family);
    if (hint) DeleteObject(hint);
    if (control) DeleteObject(control);
    if (brand) DeleteObject(brand);
    sidebar = title = heading = body = family = hint = control = brand = nullptr;
  }

  void Ensure(unsigned new_dpi) {
    if (sidebar && dpi == new_dpi) return;
    Release();
    dpi = new_dpi ? new_dpi : 96;
    auto make = [&](int dip, int weight) {
      return CreateFontW(-MulDiv(dip, static_cast<int>(dpi), 96), 0, 0, 0, weight,
                         FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                         L"Microsoft YaHei UI");
    };
    sidebar = make(13, FW_MEDIUM);
    title = make(11, FW_SEMIBOLD);
    heading = make(20, FW_SEMIBOLD);
    body = make(13, FW_NORMAL);
    family = make(11, FW_NORMAL);
    hint = make(9, FW_NORMAL);
    control = make(13, FW_NORMAL);
    brand = make(32, FW_BOLD);
  }
};

inline void PaintSettingsShell(HDC dc, const SettingsLayout& layout,
                               const SettingsDraft& draft,
                               const SettingsFonts& fonts,
                               unsigned dpi) {
  const ThemeId theme = draft.preview_theme;
  const ThemePalette& p = Palette(theme);
  RECT client{0, 0,
              static_cast<LONG>(layout.width_dip * static_cast<float>(dpi) / 96.0f +
                                0.5f),
              static_cast<LONG>(layout.height_dip * static_cast<float>(dpi) /
                                    96.0f +
                                0.5f)};
  auto px = [&](const DipRect& r) {
    RECT out{};
    out.left = static_cast<LONG>(r.left * dpi / 96.0f + 0.5f);
    out.top = static_cast<LONG>(r.top * dpi / 96.0f + 0.5f);
    out.right = static_cast<LONG>(r.right * dpi / 96.0f + 0.5f);
    out.bottom = static_cast<LONG>(r.bottom * dpi / 96.0f + 0.5f);
    return out;
  };

  FillRectColor(dc, client, ToColorRef(p.settings_background));

  FillRectColor(dc, px(layout.sidebar), ToColorRef(p.settings_background));
  RECT side_divider = px(layout.divider);
  side_divider.right = side_divider.left + 1;
  FillRectColor(dc, side_divider, ToColorRef(p.settings_separator));

  SetBkMode(dc, TRANSPARENT);
  HGDIOBJ old = SelectObject(dc, fonts.sidebar ? fonts.sidebar : GetStockObject(DEFAULT_GUI_FONT));
  SetTextColor(dc, ToColorRef(p.text_muted));
  // The native caption already owns the gear and “设置” title.

  RECT extension_header = px({12, layout.nav[5].bottom + 6, 148, layout.nav[5].bottom + 28});
  DrawTextW(dc, L"扩展", -1, &extension_header, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  for (int i = 0; i < kSettingsPageCount; ++i) {
    if (!SettingsPageAvailable(draft, static_cast<SettingsPage>(i))) continue;
    const auto row = px(layout.nav[static_cast<std::size_t>(i)]);
    const bool selected = static_cast<int>(draft.page) == i;
    if (selected) {
      FillRoundRect(dc, row, MulDiv(8, static_cast<int>(dpi), 96),
                    ToColorRef(Blend(p.settings_background, p.accent, 0.16f)));
    } else if (draft.hover == i) {
      FillRoundRect(dc, row, MulDiv(8, static_cast<int>(dpi), 96),
                    ToColorRef(p.surface_tertiary));
    }
    if (draft.keyboard_focus && draft.focus == i) {
      HPEN focus_pen =
          CreatePen(PS_SOLID, (std::max)(1, MulDiv(1, static_cast<int>(dpi), 96)),
                    ToColorRef(p.accent));
      HGDIOBJ old_pen = SelectObject(dc, focus_pen);
      HGDIOBJ old_br = SelectObject(dc, GetStockObject(NULL_BRUSH));
      RoundRect(dc, row.left, row.top, row.right, row.bottom,
                MulDiv(8, static_cast<int>(dpi), 96),
                MulDiv(8, static_cast<int>(dpi), 96));
      SelectObject(dc, old_br);
      SelectObject(dc, old_pen);
      DeleteObject(focus_pen);
    }
    RECT icon = row;
    icon.left += MulDiv(6, static_cast<int>(dpi), 96);
    icon.right = icon.left + MulDiv(18, static_cast<int>(dpi), 96);
    icon.top += (row.bottom - row.top - MulDiv(18, static_cast<int>(dpi), 96)) / 2;
    icon.bottom = icon.top + MulDiv(18, static_cast<int>(dpi), 96);
    DrawFluentIcon(dc, kSettingsPageIcons[i], icon,
                  ToColorRef(selected ? p.text_primary : p.text_secondary));
    SelectObject(dc, fonts.sidebar);
    SetTextColor(dc, ToColorRef(selected ? p.text_primary : p.text_secondary));
    RECT text = row;
    text.left = icon.right + MulDiv(8, static_cast<int>(dpi), 96);
    DrawTextW(dc, kSettingsPageTitles[i], -1, &text,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }

  // Subpage bar + divider
  FillRectColor(dc, px(layout.subpage_bar), ToColorRef(p.settings_background));
  RECT sub_div = px({layout.content.left, layout.subpage_bar.bottom - 1.0f,
                     layout.content.right, layout.subpage_bar.bottom});
  sub_div.top = sub_div.bottom - 1;
  FillRectColor(dc, sub_div, ToColorRef(p.settings_separator));
  const DipRect segments = {layout.subpage_tabs[0].left,
                            layout.subpage_tabs[0].top,
                            layout.subpage_tabs[SettingsTabCount(draft.page)-1].right,
                            layout.subpage_tabs[SettingsTabCount(draft.page)-1].bottom};
  FillRoundRect(dc, px(segments), MulDiv(5, static_cast<int>(dpi), 96),
                ToColorRef(Blend(p.settings_background, p.text_muted, 0.30f)));
  for (int i = 0; i < SettingsTabCount(draft.page); ++i) {
    const bool on = draft.subpage == i;
    RECT tab = px(layout.subpage_tabs[static_cast<std::size_t>(i)]);
    if (on) {
      FillRoundRect(dc, tab, MulDiv(5, static_cast<int>(dpi), 96),
                    ToColorRef(Blend(p.settings_background, p.text_primary, 0.35f)));
      SetTextColor(dc, ToColorRef(p.text_primary));
    } else {
      SetTextColor(dc, ToColorRef(p.text_muted));
    }
    SelectObject(dc, fonts.family);
    DrawTextW(dc, kSettingsSubpageLabels[static_cast<int>(draft.page)][i], -1,
              &tab, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }

  // Mac heading aligns to the body's leading edge, vertically centered.
  SelectObject(dc, fonts.heading);
  SetTextColor(dc, ToColorRef(p.text_primary));
  RECT heading = px(layout.heading);
  heading.left += MulDiv(24, static_cast<int>(dpi), 96);
  const auto heading_text = draft.page == SettingsPage::kHome
      ? (draft.subpage == 0 ? rimes::branding::Text(L"关于 {brand}")
                           : std::wstring(kSettingsSubpageLabels[static_cast<int>(draft.page)][draft.subpage]))
      : std::wstring(kSettingsPageTitles[static_cast<int>(draft.page)]);
  DrawTextW(dc, heading_text.c_str(), -1, &heading,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);

  if (draft.page == SettingsPage::kHome) {
    auto text = [&](const DipRect& bounds, const std::wstring& value, HFONT font, std::uint32_t color) {
      RECT rect = px(bounds);
      SelectObject(dc, font);
      SetTextColor(dc, ToColorRef(color));
      DrawTextW(dc, value.c_str(), -1, &rect, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    };
    const float x = layout.body.left, y = layout.body.top;
    if (const auto* link = HomeLink(draft.subpage)) {
      text({x, y, layout.body.right, y + 36}, rimes::branding::Text(link->title), fonts.heading, p.text_primary);
      text({x, y + 48, layout.body.right, y + 94}, rimes::branding::Text(link->summary), fonts.body, p.text_secondary);
    } else {
      FillRoundRect(dc, px(layout.home_hero), MulDiv(16, static_cast<int>(dpi), 96),
                    ToColorRef(Blend(p.settings_background, p.accent, 0.08f)));
      const int icon_size = MulDiv(88, static_cast<int>(dpi), 96);
      if (const HICON logo = CreateProductIcon(icon_size)) {
        const auto box = px({x + 16, y + 40, x + 104, y + 128});
        DrawIconEx(dc, box.left, box.top, logo, icon_size, icon_size, 0, nullptr, DI_NORMAL);
        DestroyIcon(logo);
      }
      text({x + 122, y + 16, layout.body.right - 16, y + 60}, rimes::branding::Name(), fonts.brand, p.text_primary);
      text({x + 122, y + 66, layout.body.right - 16, y + 108}, L"世界对智者太过挑剔\n好奇的人需要朋友", fonts.body, p.text_secondary);
      text({x + 122, y + 114, layout.body.right - 16, y + 152}, L"The world is too hard on the wise.\nCurious minds need friends.", fonts.family, p.text_secondary);
      text({x, layout.home_hero.bottom + 16, layout.body.right, layout.home_hero.bottom + 42},
           std::wstring(rimes::branding::Name()) + L" " + kProductVersionWide + L" · 学术海 · librime", fonts.family, p.text_muted);
    }
  } else if (draft.page == SettingsPage::kInput) {
    for (std::size_t i = 0; i < layout.scheme_cards.size(); ++i) {
      RECT card = px(layout.scheme_cards[i]);
      const bool selected = draft.schema_index == static_cast<int>(i);
      FillRoundRect(dc, card, MulDiv(8, static_cast<int>(dpi), 96),
                    ToColorRef(selected ? Blend(p.surface_secondary, p.accent, 0.10f)
                                        : p.surface_secondary));
      StrokeRoundRect(dc, card, MulDiv(8, static_cast<int>(dpi), 96),
                      ToColorRef(selected ? p.selection :
                          draft.hover == 100 + static_cast<int>(i)
                              ? p.border_strong : p.border),
                      selected ? 2 : 1);
      RECT icon = card;
      icon.left += MulDiv(10, static_cast<int>(dpi), 96);
      icon.right = icon.left + MulDiv(24, static_cast<int>(dpi), 96);
      icon.top += (card.bottom - card.top - MulDiv(24, static_cast<int>(dpi), 96)) / 2;
      icon.bottom = icon.top + MulDiv(24, static_cast<int>(dpi), 96);
      DrawFluentIcon(dc, kSettingsSchemeIcons[i], icon, ToColorRef(p.text_secondary));
      SelectObject(dc, fonts.title);
      SetTextColor(dc, ToColorRef(p.text_primary));
      RECT title = card;
      title.left = icon.right + MulDiv(9, static_cast<int>(dpi), 96);
      title.right -= MulDiv(36, static_cast<int>(dpi), 96);
      DrawTextW(dc, kSettingsSchemeTitles[i], -1, &title,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      RECT radio = card;
      radio.right -= MulDiv(12, static_cast<int>(dpi), 96);
      radio.left = radio.right - MulDiv(16, static_cast<int>(dpi), 96);
      radio.top += (card.bottom - card.top - MulDiv(16, static_cast<int>(dpi), 96)) / 2;
      radio.bottom = radio.top + MulDiv(16, static_cast<int>(dpi), 96);
      DrawFluentIcon(dc, selected ? FluentIcon::kRadioOn : FluentIcon::kRadioOff, radio,
                    ToColorRef(selected ? p.accent : p.border_strong));
      if (draft.keyboard_focus && draft.focus == 100 + static_cast<int>(i)) {
        HPEN focus_pen = CreatePen(
            PS_SOLID, (std::max)(1, MulDiv(1, static_cast<int>(dpi), 96)),
            ToColorRef(p.accent));
        HGDIOBJ old_pen = SelectObject(dc, focus_pen);
        HGDIOBJ old_br = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, card.left, card.top, card.right, card.bottom,
                  MulDiv(8, static_cast<int>(dpi), 96),
                  MulDiv(8, static_cast<int>(dpi), 96));
        SelectObject(dc, old_br);
        SelectObject(dc, old_pen);
        DeleteObject(focus_pen);
      }
    }
  } else if (draft.page == SettingsPage::kAppearance && draft.subpage == 0) {
    SelectObject(dc, fonts.title);
    SetTextColor(dc, ToColorRef(p.text_secondary));
    RECT section = px({layout.body.left, layout.body.top, layout.body.right,
                       layout.body.top + 18});
    DrawTextW(dc, L"选择主题", -1, &section,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    for (std::size_t i = 0; i < layout.theme_cards.size(); ++i) {
      const ThemeId card_theme = static_cast<ThemeId>(i);
      const ThemePalette& tp = Palette(card_theme);
      RECT card = px(layout.theme_cards[i]);
      const bool selected = draft.theme_index == static_cast<int>(i);
      FillRoundRect(dc, card, MulDiv(8, static_cast<int>(dpi), 96),
                    ToColorRef(tp.surface_secondary));
      StrokeRoundRect(dc, card, MulDiv(8, static_cast<int>(dpi), 96),
                      ToColorRef(selected ? tp.selection :
                          draft.hover == 200 + static_cast<int>(i)
                              ? tp.border_strong : tp.border),
                      selected ? 2 : 1);
      RECT preview = card;
      preview.left += MulDiv(10, static_cast<int>(dpi), 96);
      preview.right -= MulDiv(10, static_cast<int>(dpi), 96);
      preview.top += MulDiv(10, static_cast<int>(dpi), 96);
      preview.bottom = preview.top + MulDiv(68, static_cast<int>(dpi), 96);
      FillRoundRect(dc, preview, MulDiv(6, static_cast<int>(dpi), 96),
                    ToColorRef(tp.surface));
      StrokeRoundRect(dc, preview, MulDiv(6, static_cast<int>(dpi), 96),
                      ToColorRef(tp.border), 1);
      const std::uint32_t colors[3] = {
          card_theme == ThemeId::kRasta ? tp.brand_red : tp.surface_secondary,
          card_theme == ThemeId::kRasta ? tp.brand_yellow : tp.selection,
          card_theme == ThemeId::kRasta ? tp.brand_green : tp.accent};
      int sx = preview.left + MulDiv(12, static_cast<int>(dpi), 96);
      const int sy =
          (preview.top + preview.bottom) / 2 - MulDiv(15, static_cast<int>(dpi), 96);
      const int sw = (preview.right - preview.left - MulDiv(36, static_cast<int>(dpi), 96)) / 3;
      for (int s = 0; s < 3; ++s) {
        RECT swatch{sx, sy, sx + sw,
                    sy + MulDiv(30, static_cast<int>(dpi), 96)};
        FillRoundRect(dc, swatch, MulDiv(4, static_cast<int>(dpi), 96),
                      ToColorRef(colors[s]));
        sx += sw + MulDiv(6, static_cast<int>(dpi), 96);
      }
      if (selected) {
        RECT mark{preview.right - MulDiv(24, static_cast<int>(dpi), 96),
                  preview.top + MulDiv(7, static_cast<int>(dpi), 96),
                  preview.right - MulDiv(7, static_cast<int>(dpi), 96),
                  preview.top + MulDiv(24, static_cast<int>(dpi), 96)};
        InflateRect(&mark, -MulDiv(2, static_cast<int>(dpi), 96),
                    -MulDiv(2, static_cast<int>(dpi), 96));
        HBRUSH mark_brush = CreateSolidBrush(ToColorRef(tp.accent));
        const auto old_mark = SelectObject(dc, mark_brush);
        const auto old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, mark.left, mark.top, mark.right, mark.bottom);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_mark);
        DeleteObject(mark_brush);
        DrawFluentIcon(dc, FluentIcon::kCheck, mark, ToColorRef(tp.accent_foreground));
      }
      SelectObject(dc, fonts.title);
      SetTextColor(dc, ToColorRef(tp.text_primary));
      RECT name = card;
      name.left += MulDiv(12, static_cast<int>(dpi), 96);
      name.top = preview.bottom + MulDiv(8, static_cast<int>(dpi), 96);
      name.bottom = card.bottom - MulDiv(10, static_cast<int>(dpi), 96);
      DrawTextW(dc, kThemeTitles[i], -1, &name,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
      SIZE size{};
      GetTextExtentPoint32W(dc, kThemeTitles[i],
                            static_cast<int>(wcslen(kThemeTitles[i])), &size);
      SelectObject(dc, fonts.family);
      SetTextColor(dc, ToColorRef(tp.text_muted));
      RECT family = name;
      family.top += MulDiv(2, static_cast<int>(dpi), 96);
      family.left = name.left + size.cx + MulDiv(8, static_cast<int>(dpi), 96);
      DrawTextW(dc, kThemeFamilies[i], -1, &family,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
      RECT info = px(layout.theme_details[i]);
      if (draft.hover == 400 + static_cast<int>(i))
        FillRoundRect(dc, info, MulDiv(5, static_cast<int>(dpi), 96),
                      ToColorRef(tp.surface_tertiary));
      RECT glyph = info;
      InflateRect(&glyph, -MulDiv(4, static_cast<int>(dpi), 96),
                  -MulDiv(4, static_cast<int>(dpi), 96));
      DrawFluentIcon(dc, FluentIcon::kInfo, glyph, ToColorRef(tp.text_secondary));
      if (draft.keyboard_focus && draft.focus == 400 + static_cast<int>(i))
        StrokeRoundRect(dc, info, MulDiv(5, static_cast<int>(dpi), 96),
                        ToColorRef(tp.accent));
      if (draft.keyboard_focus && draft.focus == 200 + static_cast<int>(i)) {
        HPEN focus_pen = CreatePen(
            PS_SOLID, (std::max)(1, MulDiv(1, static_cast<int>(dpi), 96)),
            ToColorRef(tp.accent));
        HGDIOBJ old_pen = SelectObject(dc, focus_pen);
        HGDIOBJ old_br = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, card.left, card.top, card.right, card.bottom,
                  MulDiv(8, static_cast<int>(dpi), 96),
                  MulDiv(8, static_cast<int>(dpi), 96));
        SelectObject(dc, old_br);
        SelectObject(dc, old_pen);
        DeleteObject(focus_pen);
      }
    }
  } else if (draft.page == SettingsPage::kAppearance) {
    SelectObject(dc, fonts.body);
    SetTextColor(dc, ToColorRef(p.text_secondary));
    RECT fl = px({layout.body.left + 16, layout.font_edit.top,
                  layout.font_edit.left - 8.0f, layout.font_edit.bottom});
    DrawTextW(dc, L"候选字号（10–40）", -1, &fl,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT count_label = px({layout.body.left + 16, layout.candidate_count_edit.top,
      layout.candidate_count_edit.left - 8.f, layout.candidate_count_edit.bottom});
    DrawTextW(dc, L"每页候选数（1–9）", -1, &count_label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  } else if (draft.page == SettingsPage::kBuffer && draft.subpage == 0) {
    SelectObject(dc, fonts.body);
    SetTextColor(dc, ToColorRef(p.text_secondary));
    RECT help = px(layout.body);
    help.bottom = help.top + MulDiv(24, static_cast<int>(dpi), 96);
    DrawTextW(dc, L"唤出 / 收起 Buffer（全局，默认 Alt+Shift+B）", -1, &help,
              DT_LEFT | DT_WORDBREAK);
    help = px({layout.body.left, layout.body.top + 64, layout.body.right, layout.body.top + 88});
    DrawTextW(dc, L"发送内容（仅 Buffer 捕获时，默认 Alt+Shift+Enter）", -1, &help,
              DT_LEFT | DT_WORDBREAK);
    const wchar_t* titles[] = {L"上一个缓冲插件（默认 Alt+Shift+↑）", L"下一个缓冲插件（默认 Alt+Shift+↓）"};
    for (int i = 0; i < 2; ++i) {
      help = px({layout.body.left, layout.plugin_edit[i].top - 28, layout.body.right, layout.plugin_edit[i].top - 6});
      DrawTextW(dc, titles[i], -1, &help, DT_LEFT | DT_WORDBREAK);
    }
  } else if (draft.page == SettingsPage::kBuffer && draft.subpage == 2) {
    SelectObject(dc, fonts.body);
    SetTextColor(dc, ToColorRef(p.text_secondary));
    const wchar_t* titles[] = {L"唤出 / 关闭设置（全局）", L"唤出 / 关闭 Capsule（全局）", L"直接框选截图（全局）"};
    for (int i = 0; i < 3; ++i) {
      RECT label = px({layout.body.left, layout.global_edit[i].top - 26, layout.body.right, layout.global_edit[i].top - 3});
      DrawTextW(dc, titles[i], -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
  } else if (draft.page == SettingsPage::kBuffer) {
    SelectObject(dc, fonts.body);
    SetTextColor(dc, ToColorRef(p.text_secondary));
    for (const auto group : {DipRect{layout.body.left, layout.body.top, layout.body.right, layout.body.top + 130},
                             DipRect{layout.body.left, layout.body.top + 150, layout.body.right, layout.body.top + 306}}) {
      FillRoundRect(dc, px(group), MulDiv(10, static_cast<int>(dpi), 96), ToColorRef(p.surface_secondary));
      StrokeRoundRect(dc, px(group), MulDiv(10, static_cast<int>(dpi), 96), ToColorRef(p.border));
    }
    const wchar_t* labels[] = {L"自动发送", L"翻译方式", L"原文语言", L"目标语言"};
    const DipRect rows[] = {layout.auto_send_combo, layout.translation_combo, layout.buffer_source_language, layout.buffer_language};
    for (int i = 0; i < 4; ++i) {
      RECT label = px({layout.body.left + 18, rows[i].top, rows[i].left - 12, rows[i].bottom});
      DrawTextW(dc, labels[i], -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    RECT help = px({layout.body.left + 4, layout.body.top + 320, layout.body.right - 4, layout.body.top + 416});
    SelectObject(dc, fonts.family);
    DrawTextW(dc, L"本地翻译首次下载约 182 MiB 中英模型，之后无需联网或 API 密钥。\n"
                 L"本地支持 Chinese / English 双向翻译；其他语言可选择 AI。\n"
                 L"实时翻译工具栏可选择语言、交换方向，以及开启逐块朗读。\n\n"
                 L"切换输入框会暂停发送，重新关联后重新倒计时。发送确认完成后才移除内容。", -1, &help, DT_LEFT | DT_WORDBREAK);
  } else if (draft.page == SettingsPage::kChord) {
    const auto card = px({layout.body.left, layout.body.top - 12, layout.body.right,
                         (std::min)(layout.body.bottom, layout.body.top +
                             (draft.page == SettingsPage::kApi ? (draft.subpage == 1 ? 418.f : 384.f) : 270.f))});
    FillRoundRect(dc, card, MulDiv(10, static_cast<int>(dpi), 96), ToColorRef(p.surface_secondary));
    StrokeRoundRect(dc, card, MulDiv(10, static_cast<int>(dpi), 96), ToColorRef(p.border));
  } else if (draft.page == SettingsPage::kCapsule || draft.page == SettingsPage::kMailbox) {
    SelectObject(dc, fonts.body); SetTextColor(dc, ToColorRef(p.text_secondary));
    RECT note = px({layout.body.left,layout.body.top,layout.body.right,layout.body.top+145});
    DrawTextW(dc, draft.page == SettingsPage::kCapsule
      ? L"唤出后即可搜索和选择卡片，点击快捷栏外取消并关闭。\n双击使用卡片，复制按钮只复制并关闭。\n\n自动记录默认开启，支持文字、图片和文件；暂停期间不收录，恢复后只记录新的复制。\n\n唤出与框选截图快捷键可在 Buffer › 窗口与全局快捷键修改；录屏、OCR、多图拼接、云同步和 TOTP 尚未实现。"
      : L"终端、对话和收件在独立工作区中管理。\n\nAlt+Shift+M 打开 Mailbox；窗口内“模块”可启停工作区。关闭窗口保留终端；只有“结束”或退出输入法才停止进程。", -1, &note, DT_LEFT | DT_WORDBREAK);
    if (draft.page == SettingsPage::kCapsule) {
      RECT cleanup = px({layout.body.left,layout.body.top+256,layout.body.right,layout.body.top+374});
      DrawTextW(dc, L"自动清理\n临时历史最多 200 条、合计 1 GiB，超限优先清理较早使用的条目及无引用附件。\n新截图临时历史保留 30 天，合计最多 10 GiB。\n收藏、手动导入和正在使用的图片不会自动删除；旧版未标记来源的捕获保留。",
          -1, &cleanup, DT_LEFT | DT_WORDBREAK);
    }
  } else if (draft.page == SettingsPage::kPlugins || draft.page == SettingsPage::kTyping || draft.page == SettingsPage::kStatistics) {
    // Native extension/gallery children own this scrolling content area.
  } else {
    SelectObject(dc, fonts.body);
    SetTextColor(dc, ToColorRef(p.text_secondary));
    RECT about = px(layout.body);
    auto text = draft.about_text;
    const auto version_end = text.find(L'\n', text.find(L'\n') + 1);
    if (version_end != std::wstring::npos)
      text = draft.subpage == 0 ? text.substr(0, version_end)
                               : text.substr(version_end + 1);
    DrawTextW(dc, text.c_str(), -1, &about, DT_LEFT | DT_WORDBREAK);
    if (draft.page == SettingsPage::kAbout && draft.subpage == 0) {
      RECT note = px({layout.body.left, layout.body.top + 212, layout.body.right, layout.body.top + 302});
      DrawTextW(dc, L"重启会结束临时任务并清空临时 Buffer；学习词库、设置与收藏保留。\n已打开应用中的输入法 DLL 需由该应用重新加载。\n重新安装请运行可信的 XSIME 安装包；此处暂不提供自动重装。", -1, &note, DT_LEFT | DT_WORDBREAK);
    }
  }

  const DipRect* fields[] = {&layout.font_edit, &layout.candidate_count_edit, &layout.hotkey_edit, &layout.send_edit, &layout.plugin_edit[0], &layout.plugin_edit[1],
      &layout.base_edit, &layout.model_edit, &layout.key_edit, &layout.lang_edit,
      &layout.codex_path_edit, &layout.codex_model_edit, &layout.buffer_source_language, &layout.buffer_language};
  for (const auto* field : fields) {
    if (field->width() <= 0) continue;
    RECT box = px(*field);
    FillRoundRect(dc, box, MulDiv(8, static_cast<int>(dpi), 96),
                  ToColorRef(p.surface));
    StrokeRoundRect(dc, box, MulDiv(8, static_cast<int>(dpi), 96),
                    ToColorRef(p.border), 1);
  }
  for (const auto& field : layout.global_edit) {
    if (field.width() <= 0) continue;
    FillRoundRect(dc, px(field), MulDiv(8, static_cast<int>(dpi), 96), ToColorRef(p.surface));
    StrokeRoundRect(dc, px(field), MulDiv(8, static_cast<int>(dpi), 96), ToColorRef(p.border), 1);
  }

  SelectObject(dc, fonts.hint);
  SetTextColor(dc, ToColorRef(p.text_muted));
  RECT status = px(layout.status_bar);
  status.left += MulDiv(12, static_cast<int>(dpi), 96);
  status.right -= MulDiv(12, static_cast<int>(dpi), 96);
  const std::wstring route = std::wstring(kSettingsPageTitles[static_cast<int>(draft.page)]) +
      L" · " + kSettingsSubpageLabels[static_cast<int>(draft.page)][draft.subpage];
  SIZE route_size{};
  GetTextExtentPoint32W(dc, route.c_str(), static_cast<int>(route.size()), &route_size);
  RECT route_rect = status;
  route_rect.left = (std::max)(status.left, status.right - (std::min)(route_size.cx,
      (status.right - status.left) / 2));
  DrawTextW(dc, route.c_str(), -1, &route_rect,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
  status.right = route_rect.left - MulDiv(7, static_cast<int>(dpi), 96);
  DrawTextW(dc, draft.save_error.empty() ? L"" : L"修改无效，请检查下方提示", -1, &status,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  if (!draft.save_error.empty()) {
    RECT error = px({layout.body.left, layout.body.bottom - 28, layout.body.right, layout.body.bottom});
    SetTextColor(dc, ToColorRef(p.text_primary));
    DrawTextW(dc, draft.save_error.c_str(), -1, &error, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
  }

  if (layout.theme_popover.width() > 0 && draft.theme_detail >= 0 &&
      draft.theme_detail < 4) {
    RECT panel = px(layout.theme_popover);
    RECT shadow = panel;
    OffsetRect(&shadow, 0, MulDiv(3, static_cast<int>(dpi), 96));
    FillRoundRect(dc, shadow, MulDiv(8, static_cast<int>(dpi), 96),
                  ToColorRef(Blend(p.settings_background, 0x000000, 0.28f)));
    FillRoundRect(dc, panel, MulDiv(8, static_cast<int>(dpi), 96),
                  ToColorRef(p.surface_secondary));
    StrokeRoundRect(dc, panel, MulDiv(8, static_cast<int>(dpi), 96),
                    ToColorRef(p.border_strong));
    RECT text = panel;
    text.left += MulDiv(14, static_cast<int>(dpi), 96);
    text.right -= MulDiv(14, static_cast<int>(dpi), 96);
    text.top += MulDiv(12, static_cast<int>(dpi), 96);
    text.bottom = text.top + MulDiv(20, static_cast<int>(dpi), 96);
    SelectObject(dc, fonts.sidebar);
    SetTextColor(dc, ToColorRef(p.text_primary));
    DrawTextW(dc, kThemeTitles[draft.theme_detail], -1, &text, DT_LEFT | DT_SINGLELINE);
    OffsetRect(&text, 0, MulDiv(24, static_cast<int>(dpi), 96));
    SelectObject(dc, fonts.family);
    SetTextColor(dc, ToColorRef(p.text_muted));
    DrawTextW(dc, draft.theme_detail == 3 ? L"独立主题" : L"经典配色", -1, &text,
              DT_LEFT | DT_SINGLELINE);
    OffsetRect(&text, 0, MulDiv(20, static_cast<int>(dpi), 96));
    text.bottom = panel.bottom - MulDiv(14, static_cast<int>(dpi), 96);
    SelectObject(dc, fonts.body);
    SetTextColor(dc, ToColorRef(p.text_secondary));
    DrawTextW(dc, kThemeDetails[draft.theme_detail], -1, &text,
              DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
  }
  SelectObject(dc, old);
}

}  // namespace rimes::windows::ui
