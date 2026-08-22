#include "stats_overlay.h"
#include "test_harness.h"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>

namespace {

using osss::GeneratedFrameShare;
using osss::test::Require;
using osss::test::RequireNear;

void TestNoSamplesYet() {
    Require(GeneratedFrameShare(0, 0, 0) == 0.0, "an empty interval must report no generated share");
    Require(GeneratedFrameShare(4, 0, 0) == 0.0,
        "interpolated presents without submitted presents cannot form a share");
}

void TestUnityPassthrough() {
    // 60 FPS source at a 60 FPS target: the unity guard presents real endpoints,
    // so nothing is interpolated and nothing is generated. Measured live at 0%.
    Require(GeneratedFrameShare(0, 60, 60) == 0.0,
        "unity passthrough must report a zero generated share");
}

void TestSixtyToOneFortyFour() {
    // The headline case. 60 unique source frames paced to 144 presents: 84 of
    // those presents exist only because of generation.
    //
    // Note the first argument. Live capture puts interpolated presents at 96-99%
    // of submitted here, because an output slot practically never lands inside
    // the 1e-6 alpha snap window -- so a share taken from that count alone would
    // read 99%, not 58%. The uplift bound is what makes this correct.
    RequireNear(GeneratedFrameShare(144, 60, 144), 0.583333333, 1e-9,
        "60->144 generates seven of every twelve presents");
    RequireNear(GeneratedFrameShare(139, 60, 144), 0.583333333, 1e-9,
        "a slot that happened to snap to a real frame must not change the uplift");
}

void TestSixtyToOneTwenty() {
    RequireNear(GeneratedFrameShare(120, 60, 120), 0.5, 1e-12,
        "2x generation submits one synthesised frame per real frame");
}

void TestStallDoesNotCountAsGenerated() {
    // A stalled source holds a real frame into every slot: submitted stays high
    // while unique collapses, so the uplift term alone would claim 100%. The
    // interpolated term is the guard, because a hold never interpolates.
    Require(GeneratedFrameShare(0, 0, 240) == 0.0, "a full stall generates nothing");
    Require(GeneratedFrameShare(0, 12, 240) == 0.0, "held real frames are native, not generated");
    RequireNear(GeneratedFrameShare(60, 12, 240), 0.25, 1e-12,
        "a partial stall is bounded by what the interpolator actually produced");
}

void TestSourceSlowerThanTargetRaisesShare() {
    // Source halves to 30 while the target holds 144: more of the output is
    // synthesised than before, and the share must rise to match.
    RequireNear(GeneratedFrameShare(144, 30, 144), 0.791666666, 1e-9,
        "a slower source must raise the generated share");
}

void TestCaptureAheadOfPresentation() {
    // Unique can exceed submitted when the ceiling declines slots or presentation
    // misses them. That is not negative generation.
    Require(GeneratedFrameShare(0, 200, 144) == 0.0, "more unique than submitted clamps to zero");
    Require(GeneratedFrameShare(0, 144, 144) == 0.0, "one present per unique frame is all native");
}

void TestTornSampleClamps() {
    Require(GeneratedFrameShare(400, 0, 144) == 1.0,
        "counters read either side of a present must clamp to a full share");
}

void TestCumulativeCountsKeepPrecision() {
    // The console figure is session-cumulative, so it stays exact after hours.
    constexpr std::uint64_t submitted = 240ULL * 3600ULL * 8ULL;
    RequireNear(GeneratedFrameShare(submitted, submitted / 2, submitted), 0.5, 1e-12,
        "large cumulative counters must not lose precision");
}

} // namespace

int main() {
    try {
        TestNoSamplesYet();
        TestUnityPassthrough();
        TestSixtyToOneFortyFour();
        TestSixtyToOneTwenty();
        TestStallDoesNotCountAsGenerated();
        TestSourceSlowerThanTargetRaisesShare();
        TestCaptureAheadOfPresentation();
        TestTornSampleClamps();
        TestCumulativeCountsKeepPrecision();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << "\n";
        return EXIT_FAILURE;
    }
    std::cout << "stats overlay tests passed\n";
    return EXIT_SUCCESS;
}
