// The launcher's layout cursor and colour tokens.
//
// This is the half of the redesign that needs no window: if Column() can be
// made to hand out two rects that overlap, the launcher can be made to paint
// one control over another again, and no amount of care in gui_main.cpp would
// prevent it. osss_gui --self-test covers the other half -- that the controls
// actually created from these rects do not intersect on screen.

#include "launcher_theme.h"

#include "test_harness.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using osss::IntRect;
using osss::LauncherLayout;
using osss::LauncherPaletteFor;
using osss::RgbColor;
using osss::ShadeColor;
using osss::test::Require;

bool Intersects(const IntRect& first, const IntRect& second) {
    return first.x < second.x + second.width && second.x < first.x + first.width &&
        first.y < second.y + second.height && second.y < first.y + first.height;
}

void TestRowSpansTheContentWidthAndAdvances() {
    LauncherLayout layout(40);
    const IntRect row = layout.Row(32);
    Require(row.x == LauncherLayout::kContentLeft, "a row starts at the content left");
    Require(row.x + row.width == LauncherLayout::kContentRight, "a row ends at the content right");
    Require(row.width == LauncherLayout::kContentWidth, "a row spans the content width");
    Require(row.y == 40 && row.height == 32, "a row is placed at the cursor");
    Require(layout.Cursor() == 72, "a row advances the cursor past itself");
}

void TestColumnsDoNotAdvanceTheCursor() {
    LauncherLayout layout(100);
    const IntRect left = layout.Column(0, 2, 32);
    const IntRect right = layout.Column(1, 2, 32);
    Require(layout.Cursor() == 100, "columns share a row, so they do not advance");
    Require(left.y == 100 && right.y == 100, "columns of one row share a y");
    layout.Gap(32);
    Require(layout.Cursor() == 132, "the caller ends the row explicitly");
}

// The design's two-column grid is 326 + 20 + 326 and its three-column grid is
// 210 + 20 + 211 + 20 + 211. Both are reproduced by the remainder rule rather
// than written down, so a content-width change cannot leave them stale.
void TestGridWidthsMatchTheDesign() {
    LauncherLayout layout(0);
    Require(layout.Column(0, 1, 1).width == 672, "one column is the full content width");
    Require(layout.Column(0, 2, 1).width == 326, "two columns are 326 wide");
    Require(layout.Column(1, 2, 1).width == 326, "two columns are 326 wide");
    Require(layout.Column(1, 2, 1).x == 370, "the second of two columns starts at 370");
    Require(layout.Column(0, 3, 1).width == 210, "the first of three columns is 210");
    Require(layout.Column(1, 3, 1).width == 211, "the remainder lands on the right");
    Require(layout.Column(2, 3, 1).width == 211, "the remainder lands on the right");
}

// The property the whole cursor exists for.
void TestColumnsNeverOverlapAtAnyWidth() {
    for (int columns = 1; columns <= 6; ++columns) {
        LauncherLayout layout(0);
        std::vector<IntRect> rects;
        for (int index = 0; index < columns; ++index) {
            rects.push_back(layout.Column(index, columns, 32));
        }
        for (std::size_t first = 0; first < rects.size(); ++first) {
            for (std::size_t second = first + 1; second < rects.size(); ++second) {
                Require(
                    !Intersects(rects[first], rects[second]),
                    "columns of one row never intersect");
            }
            if (first + 1 < rects.size()) {
                Require(
                    rects[first + 1].x - (rects[first].x + rects[first].width) ==
                        LauncherLayout::kGutter,
                    "adjacent columns are exactly one gutter apart");
            }
        }
        Require(
            rects.front().x == LauncherLayout::kContentLeft &&
                rects.back().x + rects.back().width == LauncherLayout::kContentRight,
            "a column row always fills the content width exactly");
    }
}

void TestOutOfRangeColumnsAreClamped() {
    LauncherLayout layout(0);
    Require(
        layout.Column(9, 2, 32).width == layout.Column(1, 2, 32).width,
        "an out-of-range column index clamps to the last column");
    Require(
        layout.Column(0, 0, 32).width == LauncherLayout::kContentWidth,
        "a zero-column row is treated as one column rather than dividing by zero");
}

// A walk in the shape the launcher actually lays out: caption, a labelled
// two-column row, a full-width row, and a checkbox. Nothing in it may intersect.
void TestARepresentativeSectionWalkNeverOverlaps() {
    LauncherLayout layout(8);
    std::vector<IntRect> rects;

    rects.push_back(layout.Row(LauncherLayout::kCaptionHeight));
    layout.Gap(LauncherLayout::kCaptionGap);

    rects.push_back(layout.Column(0, 2, LauncherLayout::kLabelHeight));
    rects.push_back(layout.Column(1, 2, LauncherLayout::kLabelHeight));
    layout.Gap(LauncherLayout::kLabelHeight + LauncherLayout::kLabelGap);
    rects.push_back(layout.Column(0, 2, LauncherLayout::kControlHeight));
    rects.push_back(layout.Column(1, 2, LauncherLayout::kControlHeight));
    layout.Gap(LauncherLayout::kControlHeight + LauncherLayout::kRowGap);

    rects.push_back(layout.Row(LauncherLayout::kControlHeight));
    layout.Gap(LauncherLayout::kRowGap);
    rects.push_back(layout.Row(LauncherLayout::kCheckboxHeight));

    for (std::size_t first = 0; first < rects.size(); ++first) {
        for (std::size_t second = first + 1; second < rects.size(); ++second) {
            Require(!Intersects(rects[first], rects[second]), "a section walk never overlaps");
        }
    }
    Require(layout.Cursor() > rects.back().y, "the cursor ends past the last row");
}

void TestPalettesAreDistinctAndComplete() {
    const auto light = LauncherPaletteFor(false);
    const auto dark = LauncherPaletteFor(true);
    Require(light.background == RgbColor{0xF3, 0xF3, 0xF3}, "the light background is the design token");
    Require(dark.background == RgbColor{0x20, 0x20, 0x20}, "the dark background is the design token");
    Require(light.accent != dark.accent, "each theme has its own accent");
    Require(light.ok != light.failure && light.warning != light.failure, "state colours differ");
    // Contrast, cheaply: text must not be painted in the colour behind it.
    Require(light.foreground != light.background, "light text is not the light background");
    Require(dark.foreground != dark.background, "dark text is not the dark background");
    Require(dark.field != dark.field_line, "a dark field is distinguishable from its border");
}

void TestShadeMovesTowardWhiteAndBlack() {
    const RgbColor mid{100, 100, 100};
    Require(ShadeColor(mid, 50).red > 100, "a positive shade moves toward white");
    Require(ShadeColor(mid, -50).red < 100, "a negative shade moves toward black");
    Require(ShadeColor(mid, 0) == mid, "a zero shade is the identity");
    Require(ShadeColor(RgbColor{255, 255, 255}, 100) == RgbColor{255, 255, 255}, "shading clamps at white");
    Require(ShadeColor(RgbColor{0, 0, 0}, -100) == RgbColor{0, 0, 0}, "shading clamps at black");
}

} // namespace

int main() {
    try {
        TestRowSpansTheContentWidthAndAdvances();
        TestColumnsDoNotAdvanceTheCursor();
        TestGridWidthsMatchTheDesign();
        TestColumnsNeverOverlapAtAnyWidth();
        TestOutOfRangeColumnsAreClamped();
        TestARepresentativeSectionWalkNeverOverlaps();
        TestPalettesAreDistinctAndComplete();
        TestShadeMovesTowardWhiteAndBlack();
        std::cout << "OSSS launcher layout and theme-token tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
    }
    return EXIT_FAILURE;
}
