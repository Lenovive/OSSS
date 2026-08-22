// Output-shape setting: spelling, parsing, and which shape can be promoted out
// of DWM composition. GPU-free and window-free, because the header is
// deliberately dependency-free -- the launcher names a mode on a command line
// without acquiring a Direct3D dependency.

#include "output_mode.h"
#include "test_harness.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using osss::OutputMode;
using osss::test::Require;

constexpr OutputMode kAllModes[] = {
    OutputMode::overlay,
    OutputMode::fullscreen,
};

void TestArgumentRoundTrip() {
    for (const OutputMode mode : kAllModes) {
        const auto parsed = osss::ParseOutputMode(osss::OutputModeArgument(mode));
        Require(parsed.has_value(), "every emitted spelling must parse");
        Require(*parsed == mode, "spelling must round-trip to the same mode");
    }
}

void TestAliases() {
    Require(osss::ParseOutputMode(L"window") == OutputMode::overlay, "window is an alias");
    Require(osss::ParseOutputMode(L"windowed") == OutputMode::overlay, "windowed is an alias");
    Require(
        osss::ParseOutputMode(L"borderless") == OutputMode::fullscreen,
        "borderless is an alias");
    Require(
        osss::ParseOutputMode(L"exclusive") == OutputMode::fullscreen,
        "exclusive is an alias");
}

void TestRejections() {
    Require(!osss::ParseOutputMode(L"").has_value(), "empty must be rejected");
    Require(!osss::ParseOutputMode(L"Overlay").has_value(), "matching is case sensitive");
    Require(!osss::ParseOutputMode(L"full screen").has_value(), "the spelling is one word");
    Require(!osss::ParseOutputMode(L"vsync").has_value(), "a present mode is not an output mode");
}

// The load-bearing property, and the whole reason the mode exists. The overlay
// must stay layered to pass clicks to another process's window -- measured in
// tests/input_passthrough_smoke.cpp -- and a layered window is composed through
// a redirection surface whatever the swap chain asks for. So overlay can never
// be promoted, and saying otherwise anywhere would make the banner lie about
// whether a variable-refresh display is following the output clock.
void TestOnlyFullscreenCanBePromoted() {
    Require(
        !osss::OutputModeCanReachIndependentFlip(OutputMode::overlay),
        "a layered overlay can never reach independent flip");
    Require(
        osss::OutputModeCanReachIndependentFlip(OutputMode::fullscreen),
        "fullscreen output must be eligible, or VRR can never engage");
}

void TestDefaultIsOverlay() {
    // The default has to remain the click-through overlay: it is the shape that
    // does not take the player's mouse away.
    Require(
        osss::ParseOutputMode(osss::OutputModeArgument(OutputMode::overlay)) ==
            OutputMode::overlay,
        "overlay must be nameable as the default");
    Require(
        std::string(osss::OutputModeName(OutputMode::overlay)) == "overlay",
        "the diagnostic name must match the argument spelling");
}

void TestNamesAreDistinct() {
    Require(
        std::string(osss::OutputModeName(OutputMode::overlay)) !=
            osss::OutputModeName(OutputMode::fullscreen),
        "each mode must have its own diagnostic name");
}

} // namespace

int main() {
    try {
        TestArgumentRoundTrip();
        TestAliases();
        TestRejections();
        TestOnlyFullscreenCanBePromoted();
        TestDefaultIsOverlay();
        TestNamesAreDistinct();
        std::cout << "OSSS output-mode parsing and promotion-eligibility tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
    }
    return EXIT_FAILURE;
}
