#include "ui_mask.h"
#include "test_harness.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using osss::test::Require;

void TestEmptyInput() {
    const auto empty = osss::ParseUiMaskRects(std::string_view(""));
    Require(empty.Ok() && empty.rects.empty(), "empty text must parse to no regions");
    const auto whitespace = osss::ParseUiMaskRects(std::string_view(" ;\n ; "));
    Require(whitespace.Ok() && whitespace.rects.empty(), "separators alone must parse to no regions");
}

void TestFractionsAndPixels() {
    const auto parsed = osss::ParseUiMaskRects(
        std::wstring_view(L"0,0,0.25,0.2; 1600, 980, 1920, 1080 ;\n0.5,0.5,0.75,0.75PX"));
    Require(parsed.Ok(), "mixed fraction/pixel regions must parse: " + parsed.error);
    Require(parsed.rects.size() == 3, "three regions expected");
    Require(!parsed.rects[0].pixels && parsed.rects[0].right == 0.25, "first region is fractional");
    Require(parsed.rects[1].pixels && parsed.rects[1].right == 1920.0, "values above 1 imply pixels");
    Require(parsed.rects[2].pixels && parsed.rects[2].right == 0.75, "px suffix forces pixels");
}

void TestRejections() {
    Require(!osss::ParseUiMaskRects(std::string_view("0,0,0.5")).Ok(), "three values must fail");
    Require(!osss::ParseUiMaskRects(std::string_view("0,0,0.5,0.5,1")).Ok(), "five values must fail");
    Require(!osss::ParseUiMaskRects(std::string_view("a,0,0.5,0.5")).Ok(), "letters must fail");
    Require(!osss::ParseUiMaskRects(std::string_view("0.5,0,0.5,0.5")).Ok(), "zero width must fail");
    Require(!osss::ParseUiMaskRects(std::string_view("0,0.6,0.5,0.5")).Ok(), "inverted height must fail");
    Require(!osss::ParseUiMaskRects(std::string_view("-1,0,0.5,0.5")).Ok(), "negative must fail");
    Require(!osss::ParseUiMaskRects(std::string_view("0,0,0.5,0.5\"")).Ok(), "quotes must fail");
    const auto partial = osss::ParseUiMaskRects(std::string_view("0,0,0.5,0.5; bad"));
    Require(!partial.Ok() && partial.rects.empty(), "one bad region must reject the whole list");
    Require(partial.error.find("bad") != std::string::npos, "error names the offending region");
}

void TestResolveAndRasterize() {
    osss::UiMaskRect fractional{0.0, 0.0, 0.25, 0.5, false};
    const auto resolved = osss::ResolveUiMaskRect(fractional, 96, 64);
    Require(
        resolved.left == 0 && resolved.top == 0 && resolved.right == 24 && resolved.bottom == 32,
        "fractional resolve must scale to source pixels");

    osss::UiMaskRect pixels{90.0, 60.0, 500.0, 500.0, true};
    const auto clamped = osss::ResolveUiMaskRect(pixels, 96, 64);
    Require(
        clamped.left == 90 && clamped.top == 60 && clamped.right == 96 && clamped.bottom == 64,
        "pixel resolve must clamp to the frame");

    osss::UiMaskRect outside{2.0, 2.0, 3.0, 3.0, false};
    Require(osss::ResolveUiMaskRect(outside, 96, 64).Empty(), "fully outside regions resolve empty");

    const auto coverage = osss::RasterizeUiMask({fractional, pixels}, 96, 64);
    Require(coverage.size() == 96 * 64, "coverage must be source sized");
    Require(coverage[0] == 255, "top-left masked");
    Require(coverage[23] == 255 && coverage[24] == 0, "fractional right edge exclusive");
    Require(coverage[31 * 96] == 255 && coverage[32 * 96] == 0, "fractional bottom edge exclusive");
    Require(coverage[63 * 96 + 95] == 255, "clamped pixel region reaches the corner");
    Require(coverage[40 * 96 + 50] == 0, "untouched interior stays clear");
    std::size_t masked = 0;
    for (const auto value : coverage) {
        masked += value != 0 ? 1 : 0;
    }
    Require(masked == 24 * 32 + 6 * 4, "exact masked pixel count");
}

void TestFormatRoundTrip() {
    const auto parsed = osss::ParseUiMaskRects(std::string_view("0,0,0.25,0.2;1600,980,1920,1080px"));
    Require(parsed.Ok(), "round-trip input parses");
    const std::wstring formatted = osss::FormatUiMaskRects(parsed.rects);
    const auto reparsed = osss::ParseUiMaskRects(formatted);
    Require(reparsed.Ok() && reparsed.rects == parsed.rects, "formatted text must reparse identically");
}

} // namespace

int main() {
    try {
        TestEmptyInput();
        TestFractionsAndPixels();
        TestRejections();
        TestResolveAndRasterize();
        TestFormatRoundTrip();
        std::cout << "OSSS UI mask parsing, resolution, and rasterization tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
    }
    return EXIT_FAILURE;
}
