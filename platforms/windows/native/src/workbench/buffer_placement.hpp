#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace rimes::windows::workbench {

// Mirrors BufferWindowGeometry in BufferWindowController.swift. Coordinates
// are DIPs, with Windows' downward-positive Y axis, on one selected monitor.
struct BufferRect {
  double x = 0, y = 0, width = 0, height = 0;
  double right() const { return x + width; }
  double bottom() const { return y + height; }
  bool operator==(const BufferRect&) const = default;
};
enum class BufferOpeningSide { kBelow, kAbove, kBottomFallback };
struct BufferPlacement {
  BufferRect frame;
  BufferOpeningSide side = BufferOpeningSide::kBottomFallback;
};
inline constexpr double kBufferMinimumWidth = 520;
inline constexpr double kBufferMaximumWidth = 1100;
inline constexpr double kBufferDefaultWidth = 680;
inline constexpr double kBufferSafetyMargin = 8;
inline constexpr double kBufferAnchorGap = 10;
inline constexpr double kBufferFallbackBottomOffset = 120;
inline constexpr double kBufferBoxVerticalMaximumHeight = 120;

inline bool FiniteBufferRect(const BufferRect& r) {
  return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) &&
         std::isfinite(r.height) && std::isfinite(r.right()) &&
         std::isfinite(r.bottom());
}
inline bool PlausibleBufferCaret(const BufferRect& r, const BufferRect& work) {
  return FiniteBufferRect(r) && r.width >= 0 && r.height > 2 && r.height < 300 &&
         r.x >= work.x - kBufferSafetyMargin &&
         r.x <= work.right() + kBufferSafetyMargin &&
         r.y >= work.y - kBufferSafetyMargin &&
         r.y <= work.bottom() + kBufferSafetyMargin;
}
inline bool PlausibleBufferBox(const BufferRect& box, const BufferRect& caret,
                               const BufferRect& work) {
  constexpr double tolerance = 4;
  return FiniteBufferRect(box) && box.width >= 1 && box.height > 2 &&
         box.right() > work.x && box.x < work.right() &&
         box.bottom() > work.y && box.y < work.bottom() &&
         box.x - tolerance <= caret.x && box.right() + tolerance >= caret.x &&
         box.y - tolerance <= caret.y &&
         box.bottom() + tolerance >= caret.bottom();
}
inline BufferRect BufferSafeWorkArea(BufferRect work) {
  const double mx = (std::min)(kBufferSafetyMargin,
                               (std::max)(0.0, (work.width - 1) / 2));
  const double my = (std::min)(kBufferSafetyMargin,
                               (std::max)(0.0, (work.height - 1) / 2));
  return {work.x + mx, work.y + my, work.width - 2 * mx, work.height - 2 * my};
}
inline BufferPlacement BufferOpeningPlacement(
    BufferRect current, std::optional<BufferRect> caret,
    std::optional<BufferRect> box, BufferRect work, double forecast_height = 105) {
  const auto safe = BufferSafeWorkArea(work);
  if (caret && !PlausibleBufferCaret(*caret, work)) caret.reset();
  if (!caret || (box && !PlausibleBufferBox(*box, *caret, work))) box.reset();
  const double proposed = box ? box->width :
      (std::isfinite(current.width) && current.width > 0 ? current.width : kBufferDefaultWidth);
  const double width = (std::clamp)(proposed,
      (std::min)(kBufferMinimumWidth, safe.width),
      (std::min)(kBufferMaximumWidth, safe.width));
  const double height = (std::min)(
      std::isfinite(current.height) && current.height > 0 ? current.height : 73,
      safe.height);
  const double planned = (std::min)((std::max)(height, forecast_height), safe.height);
  if (!caret) {
    const double offset = (std::min)(kBufferFallbackBottomOffset,
        (std::max)(0.0, safe.height - height) / 4);
    return {{safe.x + (safe.width - width) / 2,
             safe.bottom() - offset - height, width, height},
            BufferOpeningSide::kBottomFallback};
  }
  const double x = (std::clamp)(box ? box->x : caret->x + caret->width / 2 - width / 2,
                                safe.x, (std::max)(safe.x, safe.right() - width));
  const auto anchor = box && box->height <= kBufferBoxVerticalMaximumHeight ? *box : *caret;
  const double below = (std::max)(0.0, safe.bottom() - anchor.bottom() - kBufferAnchorGap);
  const double above = (std::max)(0.0, anchor.y - kBufferAnchorGap - safe.y);
  double y;
  BufferOpeningSide side;
  if (below >= planned) {
    side = BufferOpeningSide::kBelow; y = anchor.bottom() + kBufferAnchorGap;
  } else if (above >= planned) {
    side = BufferOpeningSide::kAbove; y = anchor.y - kBufferAnchorGap - height;
  } else if (below >= height && (above < height || below >= above)) {
    side = BufferOpeningSide::kBelow; y = anchor.bottom() + kBufferAnchorGap;
  } else if (above >= height) {
    side = BufferOpeningSide::kAbove; y = anchor.y - kBufferAnchorGap - height;
  } else if (below >= above) {
    side = BufferOpeningSide::kBelow; y = safe.bottom() - height;
  } else {
    side = BufferOpeningSide::kAbove; y = safe.y;
  }
  return {{x, y, width, height}, side};
}
inline BufferRect BufferResizedOutward(BufferRect frame, double height,
                                       BufferOpeningSide side, BufferRect work) {
  const auto safe = BufferSafeWorkArea(work);
  height = (std::min)(height, safe.height);
  // Above-target and manually placed panels preserve the input-facing bottom
  // edge. Below-target panels preserve their top edge, away from the input.
  if (side != BufferOpeningSide::kBelow) frame.y += frame.height - height;
  frame.height = height;
  frame.y = (std::clamp)(frame.y, safe.y, (std::max)(safe.y, safe.bottom() - height));
  return frame;
}

inline BufferRect BufferCenteredOnScreen(BufferRect frame, BufferRect work) {
  const auto safe = BufferSafeWorkArea(work);
  frame.width = (std::min)(frame.width, safe.width);
  frame.height = (std::min)(frame.height, safe.height);
  frame.x = safe.x + (safe.width - frame.width) / 2;
  frame.y = safe.y + (safe.height - frame.height) / 2;
  return frame;
}

}  // namespace rimes::windows::workbench
