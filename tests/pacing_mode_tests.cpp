// Pacing-mode setting: spelling, parsing, and the mechanism facts each mode
// implies. GPU-free and window-free, because the header is deliberately
// dependency-free -- the launcher names a mode on a command line without
// acquiring a Direct3D dependency.

#include "pacing_mode.h"
#include "test_harness.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using osss::PacingMode;
using osss::test::Require;

constexpr PacingMode kAllModes[] = {
    PacingMode::unpaced,
    PacingMode::paced,
    PacingMode::queued,
};

void TestArgumentRoundTrip() {
    for (const PacingMode mode : kAllModes) {
        const auto parsed = osss::ParsePacingMode(osss::PacingModeArgument(mode));
        Require(parsed.has_value(), "every emitted spelling must parse");
        Require(*parsed == mode, "spelling must round-trip to the same mode");
    }
}

void TestAliases() {
    Require(osss::ParsePacingMode(L"off") == PacingMode::unpaced, "off is an alias for unpaced");
    Require(osss::ParsePacingMode(L"free") == PacingMode::unpaced, "free is an alias for unpaced");
    Require(osss::ParsePacingMode(L"on") == PacingMode::paced, "on is an alias for paced");
    Require(osss::ParsePacingMode(L"queue") == PacingMode::queued, "queue is an alias for queued");
    Require(
        osss::ParsePacingMode(L"render-ahead") == PacingMode::queued,
        "render-ahead is an alias for queued");
}

void TestRejections() {
    Require(!osss::ParsePacingMode(L"").has_value(), "empty must be rejected");
    Require(!osss::ParsePacingMode(L"Paced").has_value(), "matching is case sensitive");
    Require(!osss::ParsePacingMode(L"vsync").has_value(), "a present mode is not a pacing mode");
    Require(!osss::ParsePacingMode(L"2").has_value(), "a bare number is not a pacing mode");
}

void TestNamesAreDistinct() {
    Require(
        std::string(osss::PacingModeName(PacingMode::unpaced)) !=
            osss::PacingModeName(PacingMode::paced),
        "unpaced and paced must have their own diagnostic names");
    Require(
        std::string(osss::PacingModeName(PacingMode::paced)) !=
            osss::PacingModeName(PacingMode::queued),
        "paced and queued must have their own diagnostic names");
    Require(
        std::string(osss::PacingModeName(PacingMode::paced)) == "paced",
        "the diagnostic name must match the argument spelling");
}

// The load-bearing facts. `paced` must stay exactly the historical
// configuration -- maximum frame latency one, nothing rendered ahead, no
// lookahead -- because every measurement in the repo was taken with it, and it
// is the default. `queued` is the only mode allowed a second frame in flight,
// and it must ask the selector for exactly one slot of lookahead: fewer and it
// underruns on every slot; more and it pays latency it does not use.
void TestMechanismFacts() {
    Require(
        osss::PacingModeMaximumFrameLatency(PacingMode::paced) == 1U,
        "paced must keep maximum frame latency one");
    Require(
        osss::PacingModeMaximumFrameLatency(PacingMode::unpaced) == 1U,
        "unpaced must keep maximum frame latency one, or it queues hidden latency");
    Require(
        osss::PacingModeMaximumFrameLatency(PacingMode::queued) == 2U,
        "queued needs exactly one extra frame in flight");

    Require(!osss::PacingModeRendersAhead(PacingMode::paced), "paced renders at the deadline");
    Require(!osss::PacingModeRendersAhead(PacingMode::unpaced), "unpaced has no deadline to be ahead of");
    Require(osss::PacingModeRendersAhead(PacingMode::queued), "queued is the render-ahead mode");

    Require(osss::PacingModeUsesOutputClock(PacingMode::paced), "paced is clock-owned");
    Require(osss::PacingModeUsesOutputClock(PacingMode::queued), "queued is clock-owned");
    Require(!osss::PacingModeUsesOutputClock(PacingMode::unpaced), "unpaced has no clock");

    Require(osss::PacingModeLookaheadSlots(PacingMode::paced) == 0U, "paced needs no lookahead");
    Require(osss::PacingModeLookaheadSlots(PacingMode::unpaced) == 0U, "unpaced needs no lookahead");
    Require(osss::PacingModeLookaheadSlots(PacingMode::queued) == 1U, "queued needs one slot of lookahead");
}

// Render-ahead without the latency to hold the extra frame would block on the
// waitable object at every second slot; latency without render-ahead would let
// a paced present queue behind another. The two facts have to agree.
void TestRenderAheadImpliesLatencyTwo() {
    for (const PacingMode mode : kAllModes) {
        Require(
            osss::PacingModeRendersAhead(mode) == (osss::PacingModeMaximumFrameLatency(mode) == 2U),
            "render-ahead and maximum frame latency two must come together");
        Require(
            osss::PacingModeRendersAhead(mode) == (osss::PacingModeLookaheadSlots(mode) == 1U),
            "render-ahead and selector lookahead must come together");
    }
}

} // namespace

int main() {
    try {
        TestArgumentRoundTrip();
        TestAliases();
        TestRejections();
        TestNamesAreDistinct();
        TestMechanismFacts();
        TestRenderAheadImpliesLatencyTwo();
        std::cout << "OSSS pacing-mode parsing and mechanism tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
    }
    return EXIT_FAILURE;
}
