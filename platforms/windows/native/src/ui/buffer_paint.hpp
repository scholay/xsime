#pragma once

#include <d2d1.h>
#include <dwrite.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "buffer_layout.hpp"
#include "fluent_d2d.hpp"
#include "theme.hpp"

namespace rimes::windows::ui {

[[nodiscard]] inline D2D1_COLOR_F ColorF(std::uint32_t rgb,
                                         float a = 1.0f) noexcept {
  return D2D1::ColorF(Rf(rgb), Gf(rgb), Bf(rgb), a);
}

struct BufferPaintContext {
  ID2D1RenderTarget* target = nullptr;
  IDWriteFactory* write = nullptr;
  IDWriteTextFormat* body = nullptr;
  IDWriteTextFormat* label = nullptr;
  ID2D1SolidColorBrush* brush = nullptr;
  float dpi = 96.0f;
};

// Use the same DirectWrite metrics for horizontal scrolling and drawing.
// No wrapping: a long block remains one rail item and cannot hide its tail
// on an unpainted second line.
inline float MeasureBufferContent(IDWriteFactory* write,
                                  IDWriteTextFormat* body,
                                  const std::vector<BufferBlockView>& blocks,
                                  const std::wstring& tail,
                                  bool source_tail = true) {
  if (!write || !body) return 0;
  float width = 0;
  auto add = [&](const std::wstring& text, bool preedit) {
    if (text.empty()) return;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(write->CreateTextLayout(text.c_str(),
            static_cast<UINT32>(text.size()), body, 8000, 32, &layout)))
      return;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (preedit) layout->SetFontSize(13, {0, static_cast<UINT32>(text.size())});
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    width += metrics.widthIncludingTrailingWhitespace + 5.0f;
    layout->Release();
  };
  for (const auto& block : blocks) add(block.text, false);
  add(tail, source_tail);
  return width;
}
inline float MeasureBufferCaret(IDWriteFactory* write, IDWriteTextFormat* body,
                                const BufferPaintState& state,
                                bool composition_start = false) {
  const auto index = (std::min)(state.insertion, state.source_blocks.size());
  std::vector<BufferBlockView> prefix(state.source_blocks.begin(),
      state.source_blocks.begin() + static_cast<std::ptrdiff_t>(index));
  auto width = MeasureBufferContent(write, body, prefix, L"");
  if (!composition_start && !state.preedit.empty()) {
    const auto part = state.preedit.substr(0, state.preedit_caret);
    width += MeasureBufferContent(write, body, {}, part) - (part.empty() ? 0.0f : 5.0f);
  }
  return width;
}

inline void DrawBufferWorkbench(const BufferPaintContext& ctx,
                                const BufferPaintState& state,
                                const BufferLayout& layout) {
  auto* target = ctx.target;
  auto* write = ctx.write;
  auto* body = ctx.body;
  auto* label = ctx.label;
  auto* brush = ctx.brush;
  if (!target || !write || !body || !label || !brush) return;
  const auto icon = [&](FluentIcon id, const DipRect& box, std::uint32_t color) {
    DrawFluentIcon(target, write, brush, id,
        D2D1::RectF(box.left, box.top, box.right, box.bottom), ColorF(color));
  };

  label->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

  // Never clear the caller's full render target; host clears its canvas.
  const ThemePalette& p = Palette(state.theme);
  const float hairline = 96.0f / (std::max)(ctx.dpi, 1.0f);

  brush->SetColor(ColorF(p.buffer));
  target->FillRoundedRectangle(
      D2D1::RoundedRect(
          D2D1::RectF(layout.chrome.left, layout.chrome.top, layout.chrome.right,
                      layout.chrome.bottom),
          11.0f, 11.0f),
      brush);
  brush->SetColor(ColorF(p.buffer_border));
  target->DrawRoundedRectangle(
      D2D1::RoundedRect(
          D2D1::RectF(layout.chrome.left, layout.chrome.top, layout.chrome.right,
                      layout.chrome.bottom),
          11.0f, 11.0f),
      brush, hairline);

  // Toolbar shares buffer background (not a bright secondary stripe).
  if (!layout.toolbar_only) {
    brush->SetColor(ColorF(p.buffer_divider));
    target->DrawLine(D2D1::Point2F(layout.divider.left, layout.divider.top),
                   D2D1::Point2F(layout.divider.right, layout.divider.top),
                   brush, hairline);
  }

  auto icon_color = [&](BufferHitKind kind, bool enabled,
                        std::uint32_t normal) -> std::uint32_t {
    if (!enabled) return Blend(normal, p.buffer, 0.55f);
    if (state.pressed == static_cast<int>(kind))
      return Blend(normal, p.accent, 0.35f);
    if (state.hover == static_cast<int>(kind))
      return Blend(normal, p.text_primary, 0.25f);
    return normal;
  };

  auto draw_control_bg = [&](const DipRect& box, BufferHitKind kind,
                             bool enabled, std::uint32_t base) {
    const bool pressed = enabled && state.pressed == static_cast<int>(kind);
    const bool hovered = enabled && state.hover == static_cast<int>(kind);
    const float fill_alpha = !enabled ? 0.34f : (pressed ? 1.0f : 0.78f);
    std::uint32_t fill = Blend(base, p.surface_secondary, fill_alpha);
    if (hovered) fill = Blend(fill, p.text_primary, pressed ? 0.14f : 0.08f);
    brush->SetColor(ColorF(fill));
    const auto shape = D2D1::RoundedRect(
        D2D1::RectF(box.left + hairline * 0.5f, box.top + hairline * 0.5f,
                    box.right - hairline * 0.5f, box.bottom - hairline * 0.5f),
        6, 6);
    target->FillRoundedRectangle(shape, brush);
    brush->SetColor(ColorF(Blend(base,
        hovered ? p.border_strong : p.border, enabled ? 0.82f : 0.40f)));
    target->DrawRoundedRectangle(shape, brush, hairline);
  };

  draw_control_bg(layout.mode_button, BufferHitKind::kMode, true, p.buffer);
  icon(FluentIcon::kGrid, layout.mode_button,
              icon_color(BufferHitKind::kMode, true, p.text_secondary));

  // The mode popup shares the persistent surface and disclosure separator.
  draw_control_bg(layout.mode_chip, BufferHitKind::kMode, true, p.buffer);
  brush->SetColor(ColorF(p.border));
  const float disclosure_x = layout.mode_chip.right - 20;
  target->DrawLine(D2D1::Point2F(disclosure_x, layout.mode_chip.top + 4),
                   D2D1::Point2F(disclosure_x, layout.mode_chip.bottom - 4),
                   brush, hairline);
  const wchar_t* mode_name = L"Default";
  if (state.mode == BufferMode::kGenerate) mode_name = L"生成";
  if (state.mode == BufferMode::kTranslate) mode_name = L"实时翻译";
  if (!state.plugin_name.empty()) mode_name = state.plugin_name.c_str();
  brush->SetColor(ColorF(p.text_primary));
  target->DrawTextW(mode_name, static_cast<UINT32>(wcslen(mode_name)), label,
                    D2D1::RectF(layout.mode_chip.left + 8, layout.mode_chip.top,
                                disclosure_x - 4,
                                layout.mode_chip.bottom),
                    brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
  icon(FluentIcon::kChevronDown,
              {layout.mode_chip.right - 14, layout.mode_chip.top + 4,
               layout.mode_chip.right - 4, layout.mode_chip.bottom - 4},
              p.text_muted);

  if (layout.show_languages) {
    const bool enabled = state.translation_controls_enabled;
    auto language = [&](const DipRect& box, BufferHitKind hit, const std::wstring& title) {
      draw_control_bg(box, hit, enabled, p.buffer);
      brush->SetColor(ColorF(icon_color(hit, enabled, p.text_primary)));
      target->DrawTextW(title.c_str(), static_cast<UINT32>(title.size()), label,
          D2D1::RectF(box.left + 6, box.top, box.right - 16, box.bottom), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
      icon(FluentIcon::kChevronDown,
          {box.right - 13, box.top + 4, box.right - 3, box.bottom - 4}, p.text_muted);
    };
    language(layout.source_language, BufferHitKind::kSourceLanguage, state.source_language);
    language(layout.target_language, BufferHitKind::kTargetLanguage, state.target_language);
    draw_control_bg(layout.swap_languages, BufferHitKind::kSwapLanguages, enabled, p.buffer);
    icon(FluentIcon::kSwap, layout.swap_languages,
        icon_color(BufferHitKind::kSwapLanguages, enabled, p.text_secondary));
    if (state.hover == static_cast<int>(BufferHitKind::kSpeech) ||
        state.pressed == static_cast<int>(BufferHitKind::kSpeech))
      draw_control_bg(layout.speech, BufferHitKind::kSpeech, true, p.buffer);
    icon(state.translation_speech ? FluentIcon::kSpeakerOn : FluentIcon::kSpeakerOff, layout.speech,
        icon_color(BufferHitKind::kSpeech, true, state.translation_speech ? p.accent_text : p.text_secondary));
  }
  if (layout.show_edit) {
    draw_control_bg(layout.edit, BufferHitKind::kEdit, true, p.buffer);
    icon(FluentIcon::kArrowLeft, layout.edit, p.text_secondary);
  }
  if (layout.show_ai_choices) {
    const auto choice = [&](const DipRect& box, BufferHitKind hit, const std::wstring& title) {
      draw_control_bg(box, hit, state.ai_choices_enabled, p.buffer);
      brush->SetColor(ColorF(icon_color(hit, state.ai_choices_enabled, p.text_primary)));
      target->DrawTextW(title.c_str(), static_cast<UINT32>(title.size()), label,
          D2D1::RectF(box.left + 6, box.top, box.right - 16, box.bottom), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
      icon(FluentIcon::kChevronDown,
          {box.right - 13, box.top + 4, box.right - 3, box.bottom - 4}, p.text_muted);
    };
    choice(layout.ai_model, BufferHitKind::kAIModel, state.ai_model);
    choice(layout.ai_effort, BufferHitKind::kAIEffort, state.ai_effort);
  }
  if (layout.show_auto_send) {
    draw_control_bg(layout.auto_send, BufferHitKind::kAutoSend, true, state.auto_send_seconds ? p.accent : p.buffer);
    if (state.auto_send_seconds) {
      brush->SetColor(ColorF(p.accent_text));
      const auto timer = std::to_wstring(state.auto_send_seconds);
      label->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
      target->DrawTextW(timer.c_str(), static_cast<UINT32>(timer.size()), label,
          D2D1::RectF(layout.auto_send.left, layout.auto_send.top, layout.auto_send.right, layout.auto_send.bottom), brush);
      label->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    } else {
      icon(FluentIcon::kTimerOff, layout.auto_send, p.text_secondary);
    }
    draw_control_bg(layout.close_after, BufferHitKind::kCloseAfter, true, p.buffer);
    const auto box = layout.close_after;
    const float cy = (box.top + box.bottom) * .5f;
    brush->SetColor(ColorF(state.close_after_send ? p.accent : p.border_strong));
    target->FillRoundedRectangle(D2D1::RoundedRect({box.left, cy - 7.5f, box.right, cy + 7.5f}, 7.5f, 7.5f), brush);
    brush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
    const float cx = state.close_after_send ? box.right - 7.5f : box.left + 7.5f;
    target->FillEllipse(D2D1::Ellipse({cx, cy}, 5.5f, 5.5f), brush);
  }

  draw_control_bg(layout.close_button, BufferHitKind::kClose, true, p.buffer);
  icon(FluentIcon::kClose, layout.close_button,
              icon_color(BufferHitKind::kClose, true, p.text_secondary));

  if (layout.toolbar_only) return;
  if (layout.show_live_metrics || layout.show_reasoning) {
    brush->SetColor(ColorF(p.text_muted));
    const auto box = layout.show_reasoning ? layout.reasoning : layout.live_metrics;
    const auto text = layout.show_reasoning ? L"思考摘要 · " + state.reasoning_summary : state.live_metrics;
    target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), label,
        D2D1::RectF(box.left, box.top, box.right, box.bottom), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }

  auto paint_rail = [&](const DipRect& rail, const DipRect& text_box,
                        std::uint32_t fill,
                        const std::vector<BufferBlockView>& blocks,
                        const std::wstring& tail, bool result_lane,
                        float scroll) {
    brush->SetColor(ColorF(fill));
    target->FillRoundedRectangle(
        D2D1::RoundedRect(
            D2D1::RectF(rail.left, rail.top, rail.right, rail.bottom), 6, 6),
        brush);
    target->PushAxisAlignedClip(
        D2D1::RectF(text_box.left, text_box.top, text_box.right, text_box.bottom),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float x = text_box.left - scroll;
    if (!result_lane && state.selected) {
      brush->SetColor(ColorF(p.buffer_chip_selected));
      target->FillRectangle(D2D1::RectF(text_box.left, text_box.top + 3,
          text_box.right, text_box.bottom - 3), brush);
    }
    auto paint_item = [&](const std::wstring& text, bool streaming,
                          bool preedit, float countdown = 0) {
      if (text.empty()) return;
      IDWriteTextLayout* layout_obj = nullptr;
      if (FAILED(write->CreateTextLayout(
              text.c_str(), static_cast<UINT32>(text.size()), body, 8000.0f,
              text_box.height(), &layout_obj)))
        return;
      layout_obj->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      layout_obj->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      if (preedit)
        layout_obj->SetFontSize(13, {0, static_cast<UINT32>(text.size())});
      DWRITE_TEXT_METRICS metrics{};
      layout_obj->GetMetrics(&metrics);
      const float w = metrics.widthIncludingTrailingWhitespace;
      brush->SetColor(ColorF(p.text_primary, 1 - .65f * (std::clamp)(countdown, 0.f, 1.f)));
      target->DrawTextLayout(D2D1::Point2F(x, text_box.top), layout_obj,
                             brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
      brush->SetColor(ColorF(streaming ? p.accent : p.border_strong));
      target->DrawLine(D2D1::Point2F(x, rail.bottom - 4.0f),
                       D2D1::Point2F(x + w, rail.bottom - 4.0f), brush,
                       hairline);
      layout_obj->Release();
      x += w + 5.0f;
    };
    const auto& hint = result_lane ? state.result_hint : state.empty_hint;
    if (blocks.empty() && tail.empty() && !hint.empty()) {
      brush->SetColor(ColorF(result_lane && (hint.find(L"失败") != std::wstring::npos ||
                                           hint.find(L"错误") != std::wstring::npos) ? p.danger_text : p.text_muted));
      target->DrawTextW(hint.c_str(),
                        static_cast<UINT32>(hint.size()), label,
                        D2D1::RectF(text_box.left, text_box.top,
                                    text_box.right, text_box.bottom),
                        brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    } else {
      for (std::size_t i = 0; i <= blocks.size(); ++i) {
        if (!result_lane && i == (std::min)(state.insertion, blocks.size()) && !tail.empty())
          paint_item(tail, true, true);
        if (i < blocks.size()) paint_item(blocks[i].text, blocks[i].streaming, false, blocks[i].countdown);
      }
      if (result_lane && !tail.empty()) paint_item(tail, true, false);
    }
    if (!result_lane && state.capturing && !state.selected) {
      const auto caret_x = text_box.left - scroll + MeasureBufferCaret(write, body, state);
      brush->SetColor(ColorF(p.accent));
      target->DrawLine({caret_x, rail.top + 7}, {caret_x, rail.bottom - 7}, brush, 1.5f);
    }
    target->PopAxisAlignedClip();
  };

  if (layout.show_source) paint_rail(layout.source_rail, layout.source_text,
             layout.show_result ? p.buffer_source_rail : p.candidate,
             state.source_blocks, state.preedit, false, state.scroll_source);
  if (layout.show_result) {
    // Preview is drawn as the streaming tail only — callers must not also
    // append it into result_blocks.
    paint_rail(layout.result_rail, layout.result_text, p.buffer_target_rail,
               state.result_blocks, state.preview, true, state.scroll_result);
  }

  const auto action_surface = layout.show_result ? p.buffer_target_rail : p.candidate;
  if (layout.show_paste) {
    draw_control_bg(layout.paste, BufferHitKind::kPaste, state.paste_enabled, action_surface);
    icon(FluentIcon::kPaste, layout.paste,
                icon_color(BufferHitKind::kPaste, state.paste_enabled,
                           p.text_secondary));
  }
  if (layout.show_copy) {
    draw_control_bg(layout.copy, BufferHitKind::kCopy, state.copy_enabled, action_surface);
    icon(FluentIcon::kCopy, layout.copy,
                icon_color(BufferHitKind::kCopy, state.copy_enabled,
                           p.text_secondary));
  }
  if (layout.show_send) {
    draw_control_bg(layout.send, BufferHitKind::kSend, state.send_enabled, action_surface);
    // The primary action generates before a final exists; it only sends a
    // reviewed final afterwards. This is shared by API and local CLI routes.
    const auto primary = state.mode != BufferMode::kInput && state.result_blocks.empty()
        ? FluentIcon::kSparkle : FluentIcon::kSend;
    if (!state.busy) icon(primary, layout.send,
                icon_color(BufferHitKind::kSend, state.send_enabled,
                           p.accent_text));
  }
  if (layout.show_result && state.busy) {
    // The same primary control owns generation, waiting and delivery.
    const float cx = (layout.waiting.left + layout.waiting.right) * 0.5f;
    const float cy = (layout.waiting.top + layout.waiting.bottom) * 0.5f;
    D2D1::Matrix3x2F previous;
    target->GetTransform(&previous);
    target->SetTransform(D2D1::Matrix3x2F::Rotation(
        state.spinner_phase * 180.f / 3.14159265f, {cx, cy}) * previous);
    icon(FluentIcon::kSpinner, layout.waiting, p.accent);
    target->SetTransform(previous);
  }
  // macOS BufferInlineView uses a two-point strip at the content's bottom.
  // It is an overlay, so progress never changes the rail or text geometry.
  if (state.send_hold_progress >= 0.0f) {
    const auto& rail = layout.show_result ? layout.result_rail : layout.source_rail;
    const float amount = (std::clamp)(state.send_hold_progress, 0.0f, 1.0f);
    brush->SetColor(ColorF(p.accent));
    target->FillRoundedRectangle(D2D1::RoundedRect(
        D2D1::RectF(rail.left, rail.bottom - 2, rail.left + rail.width() * amount, rail.bottom),
        1, 1), brush);
  }
}

// Compatibility wrapper used by older call sites.
inline void DrawBufferWorkbench(ID2D1RenderTarget* target, IDWriteFactory* write,
                                IDWriteTextFormat* body,
                                IDWriteTextFormat* label,
                                ID2D1SolidColorBrush* brush,
                                const BufferPaintState& state,
                                const BufferLayout& layout) {
  DrawBufferWorkbench(BufferPaintContext{target, write, body, label, brush, 96},
                      state, layout);
}

}  // namespace rimes::windows::ui
