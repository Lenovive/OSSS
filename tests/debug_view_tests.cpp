// Diagnostic-view setting: spelling, parsing, and the shader values the fusion
// branch compares against. GPU-free, because the header is dependency-free --
// the launcher names a view on a command line without acquiring Direct3D.

#include "debug_view.h"
#include "test_harness.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using osss::DebugView;
using osss::test::Require;

constexpr DebugView kAllViews[] = {
    DebugView::off,
    DebugView::flow,
    DebugView::confidence,
    DebugView::fallback,
};

void TestArgumentRoundTrip() {
    for (const DebugView view : kAllViews) {
        const auto parsed = osss::ParseDebugView(osss::DebugViewArgument(view));
        Require(parsed.has_value(), "every emitted spelling must parse");
        Require(*parsed == view, "spelling must round-trip to the same view");
    }
}

void TestAliases() {
    Require(osss::ParseDebugView(L"none") == DebugView::off, "none is an alias");
    Require(osss::ParseDebugView(L"motion") == DebugView::flow, "motion is an alias");
    Require(osss::ParseDebugView(L"rejected") == DebugView::fallback, "rejected is an alias");
}

void TestRejections() {
    Require(!osss::ParseDebugView(L"").has_value(), "empty must be rejected");
    Require(!osss::ParseDebugView(L"Flow").has_value(), "matching is case sensitive");
    Require(!osss::ParseDebugView(L"1").has_value(), "a bare index must be rejected");
}

// The fusion shader branches on these as floats, with `> 0.5`, `< 1.5`, `< 2.5`
// tests. Off must be zero or every frame becomes a diagnostic, and the rest must
// land in distinct bands or two views collapse into one.
void TestShaderValuesSelectDistinctBranches() {
    Require(
        osss::DebugViewShaderValue(DebugView::off) == 0.0F,
        "off must be zero, or normal output becomes a diagnostic");
    for (const DebugView view : kAllViews) {
        if (view == DebugView::off) {
            continue;
        }
        Require(
            osss::DebugViewShaderValue(view) > 0.5F,
            "every active view must pass the shader's enable test");
    }
    Require(osss::DebugViewShaderValue(DebugView::flow) < 1.5F, "flow takes the first branch");
    Require(
        osss::DebugViewShaderValue(DebugView::confidence) > 1.5F &&
            osss::DebugViewShaderValue(DebugView::confidence) < 2.5F,
        "confidence takes the second branch");
    Require(
        osss::DebugViewShaderValue(DebugView::fallback) > 2.5F,
        "fallback takes the final branch");
}

void TestNamesAreDistinct() {
    for (const DebugView left : kAllViews) {
        for (const DebugView right : kAllViews) {
            if (left == right) {
                continue;
            }
            Require(
                std::string(osss::DebugViewName(left)) != osss::DebugViewName(right),
                "each view must have its own diagnostic name");
            Require(
                osss::DebugViewShaderValue(left) != osss::DebugViewShaderValue(right),
                "two views sharing a shader value would collapse into one");
        }
    }
}

} // namespace

int main() {
    try {
        TestArgumentRoundTrip();
        TestAliases();
        TestRejections();
        TestShaderValuesSelectDistinctBranches();
        TestNamesAreDistinct();
        std::cout << "OSSS diagnostic-view parsing and shader-branch tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
    }
    return EXIT_FAILURE;
}
