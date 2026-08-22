// Flow-scale setting: spelling, parsing, the divisor each setting resolves to,
// and the coarse-search reach that divisor and the source period resolve to
// together. GPU-free, because the header is deliberately dependency-free -- the
// launcher has to name a setting on a command line without acquiring a Direct3D
// dependency to do it.

#include "flow_scale.h"
#include "test_harness.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using osss::FlowScale;
using osss::test::Require;

constexpr FlowScale kAllSettings[] = {
    FlowScale::automatic,
    FlowScale::quality,
    FlowScale::performance,
    FlowScale::ultra_performance,
};

// 1920x1080 and 2560x1440 are at or under the automatic threshold; 3840x2160 is
// over it.
constexpr std::uint64_t k1080p = 1920ULL * 1080ULL;
constexpr std::uint64_t k1440p = 2560ULL * 1440ULL;
constexpr std::uint64_t k2160p = 3840ULL * 2160ULL;

// The divisors the settings resolve to. Named because the radius now depends on
// which one is in play, so every case below has to say which it means.
constexpr unsigned kDiv4 = 4U;
constexpr unsigned kDiv8 = 8U;
constexpr unsigned kDiv16 = 16U;

void TestArgumentRoundTrip() {
    for (const FlowScale setting : kAllSettings) {
        const auto parsed = osss::ParseFlowScale(osss::FlowScaleArgument(setting));
        Require(parsed.has_value(), "every emitted spelling must parse");
        Require(*parsed == setting, "spelling must round-trip to the same setting");
    }
}

void TestAliases() {
    Require(osss::ParseFlowScale(L"automatic") == FlowScale::automatic, "automatic is an alias");
    Require(osss::ParseFlowScale(L"high") == FlowScale::quality, "high is an alias");
    Require(osss::ParseFlowScale(L"fast") == FlowScale::performance, "fast is an alias");
    Require(osss::ParseFlowScale(L"ultra") == FlowScale::ultra_performance, "ultra is an alias");
    Require(osss::ParseFlowScale(L"fastest") == FlowScale::ultra_performance, "fastest is an alias");
}

void TestRejections() {
    Require(!osss::ParseFlowScale(L"").has_value(), "empty must be rejected");
    Require(!osss::ParseFlowScale(L"4").has_value(), "a bare divisor must be rejected");
    Require(!osss::ParseFlowScale(L"Quality").has_value(), "matching is case sensitive");
    Require(!osss::ParseFlowScale(L"ultra performance").has_value(), "the spelling is hyphenated");
}

// The load-bearing case: `automatic` must resolve to exactly what the pipeline
// did before this setting existed, at both sides of the threshold. Every quality
// number recorded in docs/ROADMAP.md was measured on that rule, so a change here
// silently invalidates them.
void TestAutomaticReproducesHistoricalRule() {
    Require(osss::ResolveFlowScaleDivisor(FlowScale::automatic, k1080p) == 4U, "auto is 4 at 1080p");
    Require(osss::ResolveFlowScaleDivisor(FlowScale::automatic, k1440p) == 4U, "auto is 4 at 1440p");
    Require(
        osss::ResolveFlowScaleDivisor(FlowScale::automatic, k1440p + 1ULL) == 8U,
        "auto is 8 one pixel over the threshold");
    Require(osss::ResolveFlowScaleDivisor(FlowScale::automatic, k2160p) == 8U, "auto is 8 at 4K");
}

void TestExplicitSettingsIgnoreResolution() {
    for (const std::uint64_t pixels : {k1080p, k1440p, k2160p}) {
        Require(
            osss::ResolveFlowScaleDivisor(FlowScale::quality, pixels) == 4U,
            "quality is 4 at every resolution");
        Require(
            osss::ResolveFlowScaleDivisor(FlowScale::performance, pixels) == 8U,
            "performance is 8 at every resolution");
        Require(
            osss::ResolveFlowScaleDivisor(FlowScale::ultra_performance, pixels) == 16U,
            "ultra-performance is 16 at every resolution");
    }
}

// The coarse level runs at twice the divisor and both levels index the luma
// pyramid by log2 of it, so a non-power-of-two would land the search between mip
// levels. Nothing downstream re-checks this, so it is checked here.
void TestDivisorsArePowersOfTwo() {
    for (const FlowScale setting : kAllSettings) {
        for (const std::uint64_t pixels : {k1080p, k1440p, k2160p}) {
            const unsigned divisor = osss::ResolveFlowScaleDivisor(setting, pixels);
            Require(divisor >= 4U, "no setting may go finer than the measured floor of 4");
            Require((divisor & (divisor - 1U)) == 0U, "every divisor must be a power of two");
        }
    }
}

void TestNamesAreDistinct() {
    for (const FlowScale left : kAllSettings) {
        for (const FlowScale right : kAllSettings) {
            if (left == right) {
                continue;
            }
            Require(
                std::string(osss::FlowScaleName(left)) != osss::FlowScaleName(right),
                "each setting must have its own diagnostic name");
        }
    }
}

// Source period in seconds for a rate given in frames per second, so the cases
// below read as the rates a user would recognise.
constexpr double Period(const double fps) {
    return 1.0 / fps;
}

// The load-bearing case, and the counterpart of
// TestAutomaticReproducesHistoricalRule above. A 60 FPS source on a divisor of 8
// -- what `automatic` picks above 1440p and what `performance` picks everywhere
// -- must resolve to the constants the coarse search used before reach was
// decoupled from the grid: 4 normally, 2 under performance mode. Every quality
// number in docs/ROADMAP.md and every frozen fixture in
// tests/motion_interpolator_tests.cpp was measured with those. If this fails,
// the default behaviour moved and those numbers no longer describe it.
void TestReferenceRateReproducesHistoricalRadius() {
    Require(
        osss::ResolveCoarseSearchRadius(Period(60.0), kDiv8, false) == 4,
        "a 60 FPS source on a divisor of 8 resolves to the historical radius of 4");
    Require(
        osss::ResolveCoarseSearchRadius(Period(60.0), kDiv8, true) == 2,
        "performance mode at 60 FPS on a divisor of 8 resolves to the historical radius of 2");
}

// An unmeasured period is the cold-start case: the first pairs after a start
// have no cadence yet, and must search the reference reach rather than
// collapsing to the floor.
void TestUnmeasuredPeriodResolvesToTheReference() {
    for (const double period : {0.0, -1.0, -0.004}) {
        Require(
            osss::ResolveCoarseSearchRadius(period, kDiv8, false) == 4,
            "an unmeasured period searches the reference reach");
        Require(
            osss::ResolveCoarseSearchRadius(period, kDiv8, true) == 2,
            "an unmeasured period searches the reference reach in performance mode");
        Require(
            osss::CoarseSearchPixels(
                osss::ResolveCoarseSearchRadius(period, kDiv4, false), kDiv4) ==
                static_cast<unsigned>(osss::kReferenceCoarseReachPixels),
            "cold start reaches the reference pixels on a fine grid too");
    }
}

// The point of the period half of the exercise: halving the source rate doubles
// the displacement the estimator has to find, so it must double the search.
void TestRadiusTracksTheSourcePeriod() {
    Require(
        osss::ResolveCoarseSearchRadius(Period(30.0), kDiv8, false) == 8,
        "halving the source rate doubles the search radius");
    Require(
        osss::ResolveCoarseSearchRadius(Period(120.0), kDiv8, false) == 2,
        "doubling the source rate halves the search radius");
    Require(
        osss::ResolveCoarseSearchRadius(Period(30.0), kDiv8, true) == 4,
        "performance mode stays half of the quality path at 30 FPS");
}

// THE FIX. Reach is a contract in source pixels, so choosing a finer flow grid
// must buy accuracy without spending range. Before this, the radius was counted
// in cells and `--flow-scale quality` silently halved reach against `auto`:
// 32 source pixels where a divisor of 8 got 64. That made the *less* accurate
// setting look better on fast motion, for a reason that had nothing to do with
// accuracy.
void TestReachIsIndependentOfTheFlowDivisor() {
    for (const double fps : {30.0, 60.0, 120.0}) {
        const unsigned fine = osss::CoarseSearchPixels(
            osss::ResolveCoarseSearchRadius(Period(fps), kDiv4, false), kDiv4);
        const unsigned coarse = osss::CoarseSearchPixels(
            osss::ResolveCoarseSearchRadius(Period(fps), kDiv8, false), kDiv8);
        Require(fine == coarse, "a finer flow grid must reach exactly as far, not half as far");
    }
    // The reference case stated outright, because it is the number the help text
    // and the telemetry line both quote.
    Require(
        osss::CoarseSearchPixels(
            osss::ResolveCoarseSearchRadius(Period(60.0), kDiv4, false), kDiv4) == 64U,
        "a divisor of 4 at 60 FPS reaches the reference 64 source pixels");
    // The coarsest grid agrees too, until the two-cell floor below binds.
    for (const double fps : {30.0, 60.0}) {
        Require(
            osss::CoarseSearchPixels(
                osss::ResolveCoarseSearchRadius(Period(fps), kDiv16, false), kDiv16) ==
                osss::CoarseSearchPixels(
                    osss::ResolveCoarseSearchRadius(Period(fps), kDiv8, false), kDiv8),
            "ultra-performance reaches as far as performance inside the floor");
    }
}

// Performance mode is a fraction of the reach, not a fixed radius, so it stays
// half the quality path on every grid rather than becoming the whole search on a
// coarse one.
void TestPerformanceModeHalvesReachOnEveryDivisor() {
    for (const unsigned divisor : {kDiv4, kDiv8}) {
        const unsigned full = osss::CoarseSearchPixels(
            osss::ResolveCoarseSearchRadius(Period(60.0), divisor, false), divisor);
        const unsigned half = osss::CoarseSearchPixels(
            osss::ResolveCoarseSearchRadius(Period(60.0), divisor, true), divisor);
        Require(half * 2U == full, "performance mode reaches half as far on every grid");
    }
}

// Both bounds are cost bounds, in opposite directions. The ceiling is now stated
// in pixels, so it stops a stalled source from turning the coarse pass into a
// runaway search at whatever grid is in play; the floor stops a very fast source
// from narrowing it past the point where it can seed the fine level at all.
void TestRadiusIsClamped() {
    Require(
        osss::CoarseSearchPixels(
            osss::ResolveCoarseSearchRadius(Period(4.0), kDiv8, false), kDiv8) ==
            static_cast<unsigned>(osss::kMaximumCoarseReachPixels),
        "a very slow source clamps at the reach ceiling");
    Require(
        osss::CoarseSearchPixels(
            osss::ResolveCoarseSearchRadius(Period(4.0), kDiv4, false), kDiv4) ==
            static_cast<unsigned>(osss::kMaximumCoarseReachPixels),
        "the reach ceiling is the same ceiling on a fine grid");
    Require(
        osss::ResolveCoarseSearchRadius(Period(4.0), kDiv4, false) == osss::kMaximumCoarseRadius,
        "the finest grid at the reach ceiling is the stated worst-case radius");
    Require(
        osss::ResolveCoarseSearchRadius(Period(1000.0), kDiv8, false) == osss::kMinimumCoarseRadius,
        "a very fast source clamps at the floor");
    Require(
        osss::ResolveCoarseSearchRadius(Period(1000.0), kDiv8, true) == osss::kMinimumCoarseRadius,
        "performance mode clamps at the same floor rather than below it");
    Require(
        osss::kMinimumCoarseRadius <= osss::kMaximumCoarseRadius,
        "the floor must not exceed the ceiling");
    Require(
        osss::kReferenceCoarseReachPixels <= osss::kMaximumCoarseReachPixels,
        "the reference reach must itself be inside the ceiling");
}

// A slower source can never search less far than a faster one. Sampled densely
// because the rounding inside is the only thing that could break it, and it
// would break it at one rate rather than across a range. Checked on every
// divisor, since the rounding now happens after a division by the cell size.
void TestRadiusIsMonotonicInThePeriod() {
    for (const unsigned divisor : {kDiv4, kDiv8, kDiv16}) {
        int previous = 0;
        for (double fps = 1000.0; fps >= 5.0; fps -= 0.5) {
            const int radius = osss::ResolveCoarseSearchRadius(Period(fps), divisor, false);
            Require(radius >= previous, "a longer source period must not search less far");
            previous = radius;
        }
    }
}

// What the period scaling is for, stated as the invariant rather than as the
// arithmetic: the reach in source pixels per second -- the fastest motion the
// estimator can follow -- is the same at every source rate inside the clamp.
void TestReachIsAConstantVelocityInsideTheClamp() {
    const unsigned divisor = osss::ResolveFlowScaleDivisor(FlowScale::automatic, k1080p);
    Require(divisor == kDiv4, "1080p resolves to the fine grid, which is the case worth checking");
    const double reference_velocity =
        static_cast<double>(osss::CoarseSearchPixels(
            osss::ResolveCoarseSearchRadius(Period(60.0), divisor, false), divisor)) * 60.0;
    Require(
        reference_velocity ==
            static_cast<double>(osss::kReferenceCoarseReachPixels) *
                osss::kReferenceCoarseReachSourceFps,
        "the reference reach is the reference constant at the reference rate");

    // Rates whose radius lands on an integer without rounding agree exactly.
    for (const double fps : {30.0, 40.0, 60.0, 120.0}) {
        const double velocity =
            static_cast<double>(osss::CoarseSearchPixels(
                osss::ResolveCoarseSearchRadius(Period(fps), divisor, false), divisor)) * fps;
        Require(velocity == reference_velocity, "the reach is one velocity at every clean rate");
    }

    // Everywhere else rounding to a whole cell moves it, but only by the width
    // of one cell -- never by the factor of two a fixed radius introduced
    // between 60 and 120.
    for (double fps = 32.0; fps <= 115.0; fps += 1.0) {
        const double velocity =
            static_cast<double>(osss::CoarseSearchPixels(
                osss::ResolveCoarseSearchRadius(Period(fps), divisor, false), divisor)) * fps;
        const double cell_velocity = static_cast<double>(divisor) * 2.0 * fps;
        Require(
            std::fabs(velocity - reference_velocity) <= cell_velocity,
            "rounding may move the reach by one cell, not by a factor");
    }
}

// The arithmetic `CoarseSearchPixels` implements, kept pinned separately from
// the policy above: this is the conversion the telemetry line and the help text
// both quote, and it is deliberately unchanged -- what changed is which radius
// the policy hands it.
void TestReachArithmetic() {
    Require(osss::CoarseSearchPixels(4, 4U) == 32U, "radius 4 on a divisor of 4 spans 32 pixels");
    Require(osss::CoarseSearchPixels(8, 4U) == 64U, "radius 8 on a divisor of 4 spans 64 pixels");
    Require(osss::CoarseSearchPixels(4, 8U) == 64U, "radius 4 on a divisor of 8 spans 64 pixels");
    Require(osss::CoarseSearchPixels(0, 8U) == 0U, "a zero radius reaches nothing");
}

} // namespace

int main() {
    try {
        TestArgumentRoundTrip();
        TestAliases();
        TestRejections();
        TestAutomaticReproducesHistoricalRule();
        TestExplicitSettingsIgnoreResolution();
        TestDivisorsArePowersOfTwo();
        TestNamesAreDistinct();
        TestReferenceRateReproducesHistoricalRadius();
        TestUnmeasuredPeriodResolvesToTheReference();
        TestRadiusTracksTheSourcePeriod();
        TestReachIsIndependentOfTheFlowDivisor();
        TestPerformanceModeHalvesReachOnEveryDivisor();
        TestRadiusIsClamped();
        TestRadiusIsMonotonicInThePeriod();
        TestReachIsAConstantVelocityInsideTheClamp();
        TestReachArithmetic();
        std::cout << "OSSS flow-scale parsing, divisor, and coarse-search-reach tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
    }
    return EXIT_FAILURE;
}
