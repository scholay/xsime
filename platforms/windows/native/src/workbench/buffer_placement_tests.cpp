#include "buffer_placement.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>

using namespace rimes::windows::workbench;
namespace {
int failures = 0;
void Check(bool ok, const char* message) {
  if (!ok) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
const BufferRect screen{0, 0, 1440, 900};
const BufferRect current{100, 200, 680, 73};
void TestBoxAlignment() {
  const auto short_box = BufferOpeningPlacement(current,
      BufferRect{340, 420, 0, 20}, BufferRect{300, 400, 720, 80}, screen);
  Check(short_box.frame == BufferRect{300, 490, 720, 73},
        "short field: same left/width, ten DIP below field");
  Check(short_box.side == BufferOpeningSide::kBelow, "short field below");
  const auto document = BufferOpeningPlacement(current,
      BufferRect{140, 200, 0, 20}, BufferRect{80, 100, 940, 600}, screen);
  Check(document.frame == BufferRect{80, 230, 940, 73},
        "document width follows box, vertical anchor follows caret line");
  const auto narrow = BufferOpeningPlacement(current,
      BufferRect{340, 420, 0, 20}, BufferRect{300, 400, 200, 80}, screen);
  Check(narrow.frame.x == 300 && narrow.frame.width == 520,
        "narrow input keeps left edge and readable minimum");
  const auto wide = BufferOpeningPlacement(current,
      BufferRect{140, 420, 0, 20}, BufferRect{100, 400, 2000, 80}, screen);
  Check(wide.frame.x == 100 && wide.frame.width == 1100,
        "wide input keeps left edge and maximum width");
  const auto edge = BufferOpeningPlacement(current,
      BufferRect{1320, 420, 0, 20}, BufferRect{1300, 400, 600, 80}, screen);
  Check(edge.frame.x == 832 && edge.frame.width == 600,
        "right-edge opening clamps within eight-DIP safe margin");
}
void TestCaretAndFallback() {
  const auto centered = BufferOpeningPlacement(current,
      BufferRect{720, 400, 0, 20}, {}, screen);
  Check(centered.frame.x == 380 && centered.frame.width == 680 &&
        centered.frame.y == 430, "caret-only opening preserves width and centers");
  const auto manual_width = BufferOpeningPlacement({0, 0, 900, 73},
      BufferRect{720, 400, 0, 20}, BufferRect{100, 100, 100, 50}, screen);
  Check(manual_width.frame.x == 270 && manual_width.frame.width == 900,
        "stale box is rejected; manual width is retained");
  const auto fallback = BufferOpeningPlacement({}, {}, {}, screen);
  Check(fallback.frame == BufferRect{380, 699, 680, 73} &&
        fallback.side == BufferOpeningSide::kBottomFallback,
        "missing caret defaults to 680, lower-center, 120-DIP bottom offset");
  for (const auto caret : {BufferRect{}, BufferRect{1, 1, 0, 301},
                          BufferRect{3000, 400, 0, 20},
                          BufferRect{720, 400, -1, 20},
                          BufferRect{std::numeric_limits<double>::quiet_NaN(), 400, 0, 20}}) {
    Check(BufferOpeningPlacement(current, caret, {}, screen).side ==
          BufferOpeningSide::kBottomFallback, "invalid caret never reuses stale coordinates");
  }
  const auto zero_width = BufferOpeningPlacement(current,
      BufferRect{720, 400, 0, 20}, {}, screen);
  Check(zero_width.side == BufferOpeningSide::kBelow, "zero-width caret is valid");
}
void TestStableSideAndGrowth() {
  const BufferRect work{0, 0, 1440, 800};
  const auto bottom = BufferOpeningPlacement(current,
      BufferRect{340, 760, 0, 20}, BufferRect{300, 740, 720, 60}, work);
  Check(bottom.frame.y == 657 && bottom.side == BufferOpeningSide::kAbove,
        "bottom field flips panel above");
  const auto forecast = BufferOpeningPlacement(current,
      BufferRect{720, 670, 0, 20}, {}, work);
  Check(forecast.frame.y == 587 && forecast.side == BufferOpeningSide::kAbove,
        "105-DIP forecast chooses stable above side before rail expansion");
  const auto below = BufferOpeningPlacement(current,
      BufferRect{720, 200, 0, 20}, {}, work);
  const auto taller_below = BufferResizedOutward(below.frame, 105, below.side, work);
  Check(taller_below.y == below.frame.y && taller_below.height == 105,
        "below expansion preserves top edge and grows away from input");
  const auto taller_above = BufferResizedOutward(bottom.frame, 105, bottom.side, work);
  Check(taller_above.bottom() == bottom.frame.bottom(),
        "above expansion preserves bottom edge and grows away from input");
  const auto manual = BufferResizedOutward({100, 200, 680, 73}, 105,
      BufferOpeningSide::kBottomFallback, work);
  Check(manual.bottom() == 273, "manual/fallback resizing preserves bottom edge like macOS");
  const auto tiny = BufferOpeningPlacement(current,
      BufferRect{100, 66, 0, 20}, {}, {0, 0, 360, 160});
  Check(tiny.frame.width == 344 && tiny.frame.y == 79,
        "small screen relaxes minimum and chooses larger available side");
}
void TestMonitorsAndScale() {
  const BufferRect second{-1920, -200, 1920, 1080};
  Check(BufferCenteredOnScreen({0, 0, 680, 100}, second) == BufferRect{-1300, 290, 680, 100},
        "move to current screen centers in the chosen negative-coordinate monitor");
  Check(BufferCenteredOnScreen({0, 0, 680, 100}, {0, 0, 360, 80}) == BufferRect{8, 8, 344, 64},
        "move to current screen clamps both dimensions to a small work area");
  const auto placed = BufferOpeningPlacement(current,
      BufferRect{-1750, 200, 0, 20}, BufferRect{-1800, 180, 900, 80}, second);
  Check(placed.frame.x == -1800 && placed.frame.y == 270 && placed.frame.width == 900,
        "negative-coordinate second display uses its own work area");
  for (const double scale : {1.0, 1.5, 2.0}) {
    const auto pixels = [&](BufferRect r) {
      return BufferRect{r.x * scale, r.y * scale, r.width * scale, r.height * scale};
    };
    const auto dips = [&](BufferRect r) {
      return BufferRect{r.x / scale, r.y / scale, r.width / scale, r.height / scale};
    };
    const auto result = BufferOpeningPlacement(dips(pixels(current)),
        dips(pixels({340, 420, 0, 20})), dips(pixels({300, 400, 720, 80})),
        dips(pixels(screen)));
    Check(result.frame == BufferRect{300, 490, 720, 73},
          "100/150/200 percent scale yields identical DIP placement");
  }
}
}  // namespace
int main() {
  TestBoxAlignment(); TestCaretAndFallback(); TestStableSideAndGrowth(); TestMonitorsAndScale();
  if (failures) return EXIT_FAILURE;
  std::cout << "macOS-aligned Buffer placement tests passed\n";
  return EXIT_SUCCESS;
}
