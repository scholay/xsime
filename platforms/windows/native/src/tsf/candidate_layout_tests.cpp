#include "../ui/buffer_layout.hpp"
#include "../ui/candidate_strip.hpp"
#include "../ui/theme.hpp"
#include "candidate_layout.hpp"
#include "candidate_navigation.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace rimes::windows::tsf::tests {
namespace {

using rimes::windows::ui::BufferPaintState;
using rimes::windows::ui::HitTestCandidateStrip;
using rimes::windows::ui::LayoutBuffer;
using rimes::windows::ui::LayoutCandidateStripMeasured;
using rimes::windows::ui::Palette;
using rimes::windows::ui::ParseThemeId;
using rimes::windows::ui::ScaleDip;
using rimes::windows::ui::ThemeId;
using rimes::windows::ui::ThemeIdOrDefault;
using rimes::windows::ui::CandidateStripItem;
using rimes::windows::ui::CandidateStripMetrics;
using rimes::windows::tsf::HasVisibleCaret;
using rimes::windows::tsf::ResolveCandidateCaret;

int g_failures = 0;

void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++g_failures;
  }
}

void TestPrefersBelowCaret() {
  const ScreenRect caret{100, 200, 140, 224};
  const ScreenRect work{0, 0, 1920, 1080};
  const ScreenPoint origin = PlaceCandidateWindow(caret, 320, 180, work);
  Check(origin.x == 100, "window should align with the caret left edge");
  Check(origin.y == 230, "window should sit just below the caret");
}

void TestFlipsAboveWhenNeeded() {
  const ScreenRect caret{100, 1000, 140, 1024};
  const ScreenRect work{0, 0, 1920, 1080};
  const ScreenPoint origin = PlaceCandidateWindow(caret, 320, 180, work);
  Check(origin.y == 814, "window should flip above the caret near the bottom");
  const auto vertical = PlaceCandidateWindow(caret, 320, 50, work, 6, true);
  Check(vertical.y == caret.top && vertical.x == caret.right + 6,
        "vertical first row aligns beside the caret even near the screen bottom");
  const auto edge = PlaceCandidateWindow({1900, 1070, 1900, 1080}, 320, 34, work, 6, true);
  Check(edge.y == 1046 && edge.x == 1600,
        "extreme screen edge only clamps the first row enough to remain visible");
}

void TestClampsToWorkArea() {
  const ScreenRect caret{1800, 100, 1900, 124};
  const ScreenRect work{0, 0, 1920, 1080};
  const ScreenPoint origin = PlaceCandidateWindow(caret, 400, 180, work);
  Check(origin.x == 1520, "window should stay inside the right work-area edge");
}

void TestDpiScale() {
  Check(ScaleForDpi(16, 96) == 16, "96 DPI should be identity");
  Check(ScaleForDpi(16, 144) == 24, "150% DPI should scale by 1.5");
  Check(ScaleForDpi(16, 192) == 32, "200% DPI should scale by 2");
  Check(ScaleDip(34, 96) == 34, "strip height DIP identity");
  Check(ScaleDip(34, 144) == 51, "strip height at 150%");
  Check(ScaleDip(34, 192) == 68, "strip height at 200%");
}

void TestCollapsedCaretAndSelectionLabels() {
  const auto origin = PlaceCandidateWindow({120, 180, 120, 204}, 320, 180,
                                           {0, 0, 1920, 1080});
  Check(origin.x == 120 && origin.y == 210,
        "zero-width TSF caret must remain anchored at the input field");
  Check(CandidateSelectionKey(L"", 2) == L'3',
        "unlabelled candidate must use the displayed numeric key");
  Check(CandidateSelectionKey(L"2", 0) == L'2',
        "explicit numeric label must take precedence");
  Check(CandidateSelectionKey(L"", 9) == 0 &&
            CandidateSelectionKey(L"x", 0) == 0,
        "unsupported labels must not synthesize another candidate key");
}

void TestUnknownCaretReusesPreviousPlacement() {
  // A host mid-composition can refuse every caret source, yielding an
  // all-zero rectangle. Passing that to PlaceCandidateWindow would anchor the
  // popup to the work-area origin, which is the visible jump to the screen
  // corner. The previous placement must win instead.
  ScreenRect resolved{};
  Check(ResolveCandidateCaret({420, 300, 460, 324}, {0, 0, 0, 0}, &resolved),
        "a successful query always wins");
  Check(resolved.left == 420 && resolved.bottom == 324,
        "the queried caret is used verbatim");

  Check(ResolveCandidateCaret({0, 0, 0, 0}, {420, 300, 460, 324}, &resolved),
        "an unknown caret falls back to the previous placement");
  Check(resolved.left == 420 && resolved.top == 300,
        "the popup stays next to the input field instead of jumping to (0, 0)");
  const auto origin =
      PlaceCandidateWindow(resolved, 320, 180, {0, 0, 1920, 1080});
  Check(origin.x == 420 && origin.y == 330,
        "the reused placement renders at the previous caret, not the corner");

  Check(!ResolveCandidateCaret({0, 0, 0, 0}, {0, 0, 0, 0}, &resolved),
        "no caret has ever been observed, so the popup must stay suppressed");

  Check(HasVisibleCaret({120, 180, 120, 204}),
        "a collapsed zero-width caret still identifies a visible position");
  Check(!HasVisibleCaret({0, 0, 0, 0}),
        "an all-zero rectangle is unknown geometry, not a caret");
  Check(!HasVisibleCaret({100, 200, 140, 200}),
        "a rectangle with no height carries no caret information");
  Check(!ResolveCandidateCaret({0, 0, 0, 0}, {100, 200, 100, 200}, &resolved),
        "a flat previous caret cannot stand in for a real one");
  Check(!ResolveCandidateCaret({420, 300, 460, 324}, {100, 200, 140, 224},
                               nullptr),
        "a null output is rejected rather than written");
}

void TestThemeDefaults() {
  Check(ThemeIdOrDefault("night") == ThemeId::kNight, "night parses");
  Check(ThemeIdOrDefault("day") == ThemeId::kDay, "day parses");
  Check(ThemeIdOrDefault("quiet") == ThemeId::kQuiet, "quiet parses");
  Check(ThemeIdOrDefault("rasta") == ThemeId::kRasta, "rasta parses");
  Check(ThemeIdOrDefault("unknown") == ThemeId::kNight,
        "unknown theme falls back to night");
  Check(!ParseThemeId("neon").has_value(), "unknown theme fails validation");
  Check(Palette(ThemeId::kNight).settings_background == 0x323232,
        "night settings background");
  Check(Palette(ThemeId::kDay).settings_background == 0xECECEC,
        "day settings background");
  Check(Palette(ThemeId::kNight).selection == 0x15803D, "night selection");
  Check(Palette(ThemeId::kDay).selection == 0x0F6A3F, "day selection");
}

void TestHorizontalSingleRowAndHits() {
  CandidateStripMetrics metrics;
  std::vector<CandidateStripItem> items;
  for (int i = 0; i < 9; ++i) {
    CandidateStripItem item;
    item.label = std::to_wstring(i + 1);
    item.text = L"候选项内容偏长";
    item.content_width_dip = 90;
    items.push_back(item);
  }
  const auto layout =
      LayoutCandidateStripMeasured(items, metrics, 320.0f);
  Check(layout.row_count > 1 && layout.height_dip == metrics.strip_height_dip * layout.row_count,
        "narrow monitors wrap whole phrases instead of truncating every word");
  Check(layout.pills.size() == 9, "single row keeps every selectable entry");
  Check(layout.width_dip <= 320.0f + 0.01f, "width clamps to available");
  for (std::size_t i = 0; i < layout.pills.size(); ++i) {
    const auto& pill = layout.pills[i];
    Check(pill.width() == 102.f,
          "every phrase retains its full measured width");
    if (i > 0 && pill.top == layout.pills[i - 1].top)
      Check(pill.left > layout.pills[i - 1].right,
            "single-row hit targets stay in page order without overlap");
    const float cx = (pill.left + pill.right) * 0.5f;
    const float cy = (pill.top + pill.bottom) * 0.5f;
    Check(HitTestCandidateStrip(layout, cx, cy) == static_cast<int>(i),
          "painted pill must map to the same logical index");
  }
  Check(HitTestCandidateStrip(layout, -1, -1) < 0, "miss outside returns -1");
  const auto wide = LayoutCandidateStripMeasured(items, metrics, 1920.f);
  Check(wide.row_count == 1 && wide.width_dip > metrics.max_strip_width_dip,
        "horizontal strip grows past the old wrap threshold when space permits");
  for (const auto& pill : wide.pills)
    Check(pill.width() == 102.f, "available space preserves natural candidate widths");
}

void TestLongTextDoesNotDropIndex() {
  CandidateStripMetrics metrics;
  std::vector<CandidateStripItem> items(3);
  items[0].content_width_dip = 40;
  items[1].content_width_dip = 800;
  items[2].content_width_dip = 40;
  const auto layout =
      LayoutCandidateStripMeasured(items, metrics, 460.0f);
  Check(layout.pills.size() == 3, "long text still yields three hit targets");
  Check(layout.row_count == 3, "oversized candidate receives a row without squeezing neighbors");
  Check(layout.pills[1].width() == 452.f, "only text wider than the monitor is clipped");
  Check(layout.pills[0].width() == 52.f && layout.pills[2].width() == 52.f,
        "long candidate leaves short neighbors at their natural width");
  for (const auto& pill : layout.pills)
    Check(pill.left >= 0 && pill.right <= layout.width_dip,
          "long candidates stay inside their actual clickable viewport");
  Check(HitTestCandidateStrip(
            layout, (layout.pills[1].left + layout.pills[1].right) * 0.5f,
            (layout.pills[1].top + layout.pills[1].bottom) * 0.5f) == 1,
        "page index 1 remains hittable after clip");
}

void TestNarrowViewportAndLargeFont() {
  for (const float width : {16.f, 100.f, 240.f, 460.f}) {
    const auto metrics = ui::MakeCandidateMetrics(40);
    std::vector<CandidateStripItem> items(3);
    items[0].content_width_dip = 800;
    items[1].content_width_dip = 32;
    items[2].content_width_dip = 48;
    const auto layout = LayoutCandidateStripMeasured(items, metrics, width);
    Check(layout.width_dip <= width, "small work area bounds the popup");
    Check(layout.row_count > 1 && layout.height_dip == metrics.strip_height_dip * layout.row_count,
          "large phrases wrap while preserving font size");
    Check(metrics.pill_height_dip >= metrics.candidate_font_dip + 4,
          "large text fits inside the pill");
    for (std::size_t i = 0; i < layout.pills.size(); ++i) {
      const auto& r = layout.pills[i];
      Check(r.left >= 0 && r.right <= layout.width_dip &&
                r.top >= 0 && r.bottom <= layout.height_dip,
            "every hit rectangle is visible inside the popup");
      for (const unsigned dpi : {96U, 144U, 192U}) {
        const float scale = static_cast<float>(dpi) / 96.f;
        const float x = (r.left + r.right) * .5f * scale;
        const float y = (r.top + r.bottom) * .5f * scale;
        Check(HitTestCandidateStrip(layout, x / scale, y / scale) == static_cast<int>(i),
              "DPI conversion preserves logical candidate identity");
      }
    }
    Check(HitTestCandidateStrip(layout, layout.width_dip + 1, layout.pills[0].top + 1) < 0,
          "invisible candidate text cannot receive a click");
  }
  const auto empty = LayoutCandidateStripMeasured({}, {}, 100);
  Check(empty.row_count == 0 && empty.height_dip == 0 && empty.width_dip == 0,
        "no candidates means no popup surface");
  const auto origin = PlaceCandidateWindow({100, 100, 100, 124}, 200, 100,
                                          {0, 0, 1920, 1080}, ScaleDip(6, 144));
  Check(origin.y == 133, "caret gap scales to nine pixels at 150 percent");
}

void TestVerticalCandidatesKeepEveryPageItem() {
  std::vector<CandidateStripItem> edge_items(9);
  for (auto& item : edge_items) item.content_width_dip = 90;
  const auto edge = LayoutCandidateStripMeasured(edge_items, {}, 1920.f, true, 34.f);
  Check(edge.row_count == 1 && edge.width_dip > 460.f,
        "bottom-edge columns use monitor space rather than squeezing into one column's width");
  for (const auto& pill : edge.pills)
    Check(pill.width() == 102.f, "vertical overflow columns preserve whole phrases when space permits");
  for (const float height : {180.f, 900.f}) {
    const auto metrics = ui::MakeCandidateMetrics(40);
    std::vector<CandidateStripItem> items(9);
    for (auto& item : items) item.content_width_dip = 250;
    const auto layout = LayoutCandidateStripMeasured(items, metrics, 460.f, true, height);
    Check(layout.pills.size() == 9 && layout.height_dip <= height && layout.width_dip <= 460.f,
          "vertical pages preserve all items within monitor geometry");
    for (std::size_t i = 0; i < layout.pills.size(); ++i) {
      const auto& r = layout.pills[i];
      Check(HitTestCandidateStrip(layout, (r.left+r.right)*.5f, (r.top+r.bottom)*.5f) == static_cast<int>(i),
            "vertical or overflow-column hit retains the engine index");
    }
    Check(layout.pills[1].top > layout.pills[0].top,
          "vertical reading order starts downwards");
  }
}

void TestCompactSubpages() {
  std::vector<CandidateStripItem> items(12);
  for (auto& item : items) item.content_width_dip = 90;
  items[5].content_width_dip = 1200;
  CandidateStripMetrics metrics;
  metrics.max_strip_width_dip = 320;
  const auto first = ui::LayoutCandidateCompactMeasured(items, metrics, 1920, 3, 0, true);
  Check(first.pages.size() > 4 && first.row_count == 1 && first.width_dip <= 320,
        "ordinary horizontal pages obey the configured ceiling and count, not monitor width");
  Check(first.previous.width() > 0 && first.next.width() > 0,
        "pagination reserves both controls before measuring visible items");
  std::size_t seen = 0;
  for (std::size_t page = 0; page < first.pages.size(); ++page) {
    const auto range = first.pages[page];
    Check(range.begin == seen && range.end > range.begin && range.end - range.begin <= 3,
          "subpages partition every engine candidate exactly once");
    seen = range.end;
    for (auto selected = range.begin; selected < range.end; ++selected) {
      const auto layout = ui::LayoutCandidateCompactMeasured(items, metrics, 1920, 3, selected, true);
      Check(layout.page_index == page && layout.pages.size() == first.pages.size() &&
                layout.height_dip == metrics.strip_height_dip && layout.width_dip <= 320,
            "highlight changes reveal a stable single-row subpage without shifting boundaries");
      for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& box = layout.pills[i];
        Check((box.width() > 0) == (i >= range.begin && i < range.end),
              "only this subpage has visible and clickable candidates");
        if (box.width() > 0)
          Check(HitTestCandidateStrip(layout, (box.left + box.right) / 2,
                    (box.top + box.bottom) / 2) == static_cast<int>(i),
                "subpage clicks retain the engine index, including a clipped long phrase");
      }
    }
  }
  Check(seen == items.size(), "last short subpage keeps the complete suffix");
  items.assign(9, {});
  for (auto& item : items) item.content_width_dip = 10;
  const auto limited = ui::LayoutCandidateCompactMeasured(items, {}, 1920, 4, 0, false);
  Check(limited.pages.size() == 3 && limited.pages[0].end == 4 && limited.pages[1].end == 8,
        "candidate-count bound applies even when every candidate fits the width");
  items.resize(1);
  const auto terminal = ui::LayoutCandidateCompactMeasured(items, {}, 1920, 9, 0, false);
  Check(terminal.pages.size() == 1 && terminal.next.width() == 0,
        "one terminal subpage needs no paging controls");
  for (const float width : {16.f, 100.f, 240.f}) {
    const auto narrow = ui::LayoutCandidateCompactMeasured(items, {}, width, 9, 0, true);
    Check(narrow.width_dip <= width && narrow.pills[0].width() > 0 &&
        narrow.next.right <= width && narrow.pills[0].right <= narrow.previous.left,
        "tiny work areas retain separate bounded candidate and pager hit targets");
  }
}

void TestBufferLayoutBounds() {
  BufferPaintState state;
  state.source_blocks = {{L"a", false}};
  auto layout = LayoutBuffer(state, 400);
  Check(layout.width_dip == 520, "buffer width floors at 520");
  layout = LayoutBuffer(state, 2000);
  Check(layout.width_dip == 1100, "buffer width caps at 1100");
  layout = LayoutBuffer(state, 760);
  Check(layout.height_dip == 89 && layout.show_live_metrics,
        "Default expanded height includes its live typing metrics");
  state.translate = true;
  state.mode = rimes::windows::ui::BufferMode::kTranslate;
  state.result_blocks = {{L"b", false}};
  layout = LayoutBuffer(state, 760);
  Check(layout.height_dip == 105, "translation expanded height");
  BufferPaintState empty;
  layout = LayoutBuffer(empty, 760);
  Check(layout.height_dip == 89 && layout.show_live_metrics,
        "empty Default keeps its source rail and live typing metrics");
  empty.folded = true;
  layout = LayoutBuffer(empty, 760);
  Check(layout.height_dip == 35, "toolbar-only height");
  Check(!layout.show_source && !layout.show_result && !layout.show_edit,
        "folded workbench exposes no hidden rail or candidate caret surface");
}

void TestCandidateMatrixNavigation() {
  std::vector<BrokerInputState> pages(6);
  for (std::size_t row = 0; row < pages.size(); ++row) {
    auto& page = pages[row];
    page.candidates_visible = page.composing = true;
    page.composition = L"ni";
    page.page_start = 0;
    page.page_size = row == 5 ? 2 : 9;
    page.candidates.resize(row == 5 ? 2 : 9);
    for (std::size_t index = 0; index < page.candidates.size(); ++index)
      page.candidates[index].id = static_cast<std::uint32_t>(row * 9 + index);
  }
  std::size_t engine_row = 0;
  auto step = [&](int delta) -> std::optional<BrokerInputState> {
    engine_row = static_cast<std::size_t>((std::clamp)(
        static_cast<int>(engine_row) + delta, 0, 5));
    return pages[engine_row];
  };
  CandidateNavigation navigation;
  navigation.Reset(pages[0]);
  navigation.MoveColumn(1);
  Check(navigation.column() == 1 && engine_row == 0,
        "horizontal arrows select a word without editing the engine preedit");
  Check(navigation.MoveRow(1, step) && navigation.expanded() &&
            navigation.end() == 3 && engine_row == 0 && navigation.column() == 1,
        "Down previews three pages and restores the selected engine page");
  Check(navigation.MoveRow(-1, step) && !navigation.expanded() && engine_row == 0,
        "Up on the first matrix row collapses without cancelling composition");
  Check(navigation.MoveRow(1, step), "matrix opens again");
  Check(navigation.SelectVisibleIndex(19, step) && navigation.row() == 2 &&
            navigation.column() == 1 && engine_row == 2,
        "host selection across preview pages synchronizes the engine");
  Check(navigation.MoveRow(-1, step) && navigation.row() == 1 && engine_row == 1,
        "keyboard navigation after host selection starts on the selected page");
  Check(navigation.SelectVisibleIndex(1, step) && engine_row == 0,
        "host can select back across pages");
  Check(!navigation.SelectVisibleIndex(100, step) && engine_row == 0,
        "invalid host selection does not move the engine");
  for (std::size_t row = 1; row <= 5; ++row) {
    Check(navigation.MoveRow(1, step) && navigation.row() == row && engine_row == row,
          "Down continues beyond the first three pages");
    Check(navigation.end() - navigation.base() <= 3,
          "matrix viewport shows at most three rows");
  }
  navigation.MoveColumn(8);
  Check(navigation.column() == 1 && navigation.Selected()->page_id == 45,
        "short final page clamps selection to an existing candidate");
  Check(!navigation.Selected(8), "unavailable matrix number never selects a hidden word");
  Check(navigation.AtVisibleIndex(0)->page_id == 27 &&
            navigation.AtVisibleIndex(18)->page_id == 45,
        "mouse hit index maps to the visible page and its local index");
  Check(navigation.MoveRow(1, step) && engine_row == 5 && navigation.row() == 5,
        "end of candidate list never wraps or duplicates rows");
  Check(navigation.CollapseToAnchor(step) && engine_row == 0 && !navigation.expanded(),
        "Escape restores the opening page without committing composition");
  navigation.Reset({});
  Check(navigation.empty() && !navigation.expanded() && !navigation.Selected(),
        "commit or context retirement clears matrix selection");

  std::vector<CandidateStripItem> items(20);
  for (auto& item : items) item.content_width_dip = 90;
  const auto matrix = ui::LayoutCandidateMatrixMeasured(items, {9, 9, 2}, {}, 320);
  Check(matrix.row_count == 3 && matrix.width_dip <= 320 && matrix.pills.size() == 20,
        "matrix keeps explicit page boundaries while fitting monitor width");
  for (const auto i : matrix.order) {
    const auto& pill = matrix.pills[i];
    Check(HitTestCandidateStrip(matrix, (pill.left + pill.right) / 2,
               (pill.top + pill.bottom) / 2) == static_cast<int>(i),
          "matrix paint geometry preserves every mouse selection index");
  }
  Check(matrix.order.size() < items.size(), "long rows expose a viewport instead of squeezing every candidate");
  for (std::size_t selected = 0; selected < items.size(); ++selected) {
    const auto moved = ui::LayoutCandidateMatrixMeasured(items, {9, 9, 2}, {}, 320, selected);
    Check(moved.pills[selected].width() > 0, "arrow navigation reveals every overflow candidate");
    Check(moved.pills[selected].width() >= 90, "normal long candidates retain their measured width");
  }
}

}  // namespace

int RunCandidateLayoutTests() {
  TestPrefersBelowCaret();
  TestFlipsAboveWhenNeeded();
  TestClampsToWorkArea();
  TestDpiScale();
  TestCollapsedCaretAndSelectionLabels();
  TestUnknownCaretReusesPreviousPlacement();
  TestThemeDefaults();
  TestHorizontalSingleRowAndHits();
  TestLongTextDoesNotDropIndex();
  TestNarrowViewportAndLargeFont();
  TestVerticalCandidatesKeepEveryPageItem();
  TestCandidateMatrixNavigation();
  TestCompactSubpages();
  TestBufferLayoutBounds();
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace rimes::windows::tsf::tests

int main() {
  return rimes::windows::tsf::tests::RunCandidateLayoutTests();
}
