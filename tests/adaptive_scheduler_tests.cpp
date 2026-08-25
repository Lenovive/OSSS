#include "adaptive_scheduler.h"
#include "frame_rate_limits.h"
#include "test_harness.h"

#include <chrono>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

using Clock = osss::OutputClock::Clock;
using namespace std::chrono_literals;

using osss::test::Require;
using osss::test::RequireNear;

struct ScenarioResult {
    int measured_deadlines = 0;
    int submissions = 0;
    int fractional_interpolations = 0;
    int ceiling_holds = 0;
    int underruns = 0;
    std::uint64_t missed = 0;
};

ScenarioResult RunSteadyScenario(
    const double source_fps,
    const osss::FrameRate target_rate,
    const int maximum_multiplier,
    const Clock::duration source_phase = Clock::duration::zero(),
    const Clock::duration measured = 10s,
    const osss::FrameSelector::CeilingPacing ceiling_pacing =
        osss::FrameSelector::CeilingPacing::spread) {
    osss::OutputClock output(target_rate);
    osss::SourceTimeline timeline;
    osss::FrameSelector selector(
        maximum_multiplier,
        std::chrono::milliseconds{osss::FrameSelector::kDefaultBufferMilliseconds},
        ceiling_pacing);
    constexpr auto step = 250us;
    constexpr auto warmup = 2s;
    constexpr auto capture_delay = 2ms;
    const Clock::time_point start{};
    output.Start(start);

    const Clock::duration source_period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / source_fps));
    Clock::time_point next_media = start + source_phase;
    Clock::time_point next_arrival = next_media + capture_delay;
    std::uint64_t raw_sequence = 0;
    std::uint64_t previous_pair = 0;
    ScenarioResult result{};

    for (Clock::time_point now = start; now <= start + warmup + measured; now += step) {
        while (next_arrival <= now) {
            timeline.Ingest(
                ++raw_sequence,
                next_media,
                next_arrival,
                1920,
                1080,
                false);
            next_media += source_period;
            next_arrival = next_media + capture_delay;
        }

        if (const auto deadline = output.TakeLatestDue(now)) {
            const osss::FrameSelection selection = selector.Select(*deadline, target_rate, timeline);
            if (now >= start + warmup) {
                ++result.measured_deadlines;
                if (selection.submit) {
                    ++result.submissions;
                }
                if (selection.mode == osss::FrameSelectionMode::interpolate) {
                    Require(selection.alpha > 0.0F && selection.alpha < 1.0F,
                        "interpolated alpha must be fractional and bounded");
                    ++result.fractional_interpolations;
                }
                if (selection.mode == osss::FrameSelectionMode::ceiling_hold) {
                    ++result.ceiling_holds;
                }
                if (selection.mode == osss::FrameSelectionMode::underrun) {
                    ++result.underruns;
                }
                // Holds and fallbacks report the newest frame, not a bracket, so
                // only bracketed selections participate in the monotonic check.
                const bool bracketed =
                    selection.mode == osss::FrameSelectionMode::interpolate ||
                    selection.mode == osss::FrameSelectionMode::real_frame;
                if (bracketed && selection.previous_unique_sequence != 0) {
                    Require(selection.previous_unique_sequence >= previous_pair,
                        "selected source pairs must advance monotonically");
                    previous_pair = selection.previous_unique_sequence;
                }
            }
        }
    }
    result.missed = output.MissedDeadlineCount();
    return result;
}

using SelectionRecord = std::tuple<
    std::uint64_t,
    osss::FrameSelectionMode,
    std::uint64_t,
    std::uint64_t,
    int>;

std::vector<SelectionRecord> RunArrivalJitterScenario(const std::vector<int>& jitter_microseconds) {
    const osss::FrameRate target{144, 1};
    osss::OutputClock output(target);
    osss::SourceTimeline timeline;
    osss::FrameSelector selector(6);
    const Clock::time_point start{};
    output.Start(start);

    constexpr auto source_period = 16666667ns;
    constexpr auto base_capture_delay = 4ms;
    constexpr auto step = 250us;
    std::size_t source_index = 0;
    Clock::time_point next_media = start;
    Clock::time_point next_arrival =
        next_media + base_capture_delay +
        std::chrono::microseconds(jitter_microseconds.front());
    std::uint64_t raw_sequence = 0;
    std::vector<SelectionRecord> records;

    for (Clock::time_point now = start; now <= start + 3s; now += step) {
        while (next_arrival <= now) {
            timeline.Ingest(++raw_sequence, next_media, next_arrival, 1280, 720, false);
            ++source_index;
            next_media = start + source_period * static_cast<std::int64_t>(source_index);
            next_arrival =
                next_media + base_capture_delay +
                std::chrono::microseconds(
                    jitter_microseconds[source_index % jitter_microseconds.size()]);
        }
        if (const auto deadline = output.TakeLatestDue(now)) {
            const auto selection = selector.Select(*deadline, target, timeline);
            if (deadline->time >= start + 1s) {
                records.emplace_back(
                    deadline->slot,
                    selection.mode,
                    selection.previous_unique_sequence,
                    selection.current_unique_sequence,
                    static_cast<int>(std::lround(selection.alpha * 1'000'000.0F)));
            }
        }
    }
    return records;
}

void TestRateLimits() {
    for (int multiplier = osss::kMinimumMultiplier; multiplier <= osss::kMaximumMultiplier;
         ++multiplier) {
        Require(osss::IsValidMultiplier(multiplier), "2x through 20x must be valid");
    }
    Require(!osss::IsValidMultiplier(osss::kMinimumMultiplier - 1), "1x must be rejected");
    Require(!osss::IsValidMultiplier(osss::kMaximumMultiplier + 1), "one past the ceiling must be rejected");
    Require(osss::IsValidTargetFps(240.0), "240 FPS must be a valid target");
    Require(osss::IsValidTargetFps(osss::kMinimumTargetFps) &&
            osss::IsValidTargetFps(osss::kMaximumTargetFps),
        "24 and 1000 FPS must be accepted as inclusive bounds");
    Require(!osss::IsValidTargetFps(0.0), "zero FPS must be rejected");
    Require(!osss::IsValidTargetFps(2000.0), "2000 FPS must be rejected");

    bool invalid_multiplier_threw = false;
    try {
        osss::FrameSelector invalid(osss::kMaximumMultiplier + 1);
    } catch (const std::out_of_range&) {
        invalid_multiplier_threw = true;
    }
    Require(invalid_multiplier_threw, "invalid multiplier construction must throw");

    bool invalid_target_threw = false;
    try {
        [[maybe_unused]] const auto invalid = osss::FrameRate::FromFps(2000.0);
    } catch (const std::out_of_range&) {
        invalid_target_threw = true;
    }
    Require(invalid_target_threw, "invalid target construction must throw");
}

// Historical target/ceiling count contract, carried forward from the retired
// per-pair FramePacer test: one measured second after a two-second warmup at a
// 240 Hz target, +/-3 submissions.
// The ceiling moved from 6 to 20 to match what external frame generators
// offer. This is a separate test on purpose: TestHistoricalTargetCounts is a
// frozen fixture and must keep describing the behaviour it was written for.
//
// What is checked here is that the *scheduler* treats a high ceiling as an
// ordinary bound -- the interesting failure would be a ceiling so high it stops
// binding at all, so that a starved source silently produces held frames
// instead of an honest ceiling_hold.
void TestHighMultiplierCeiling() {
    for (int multiplier = osss::kMinimumMultiplier; multiplier <= osss::kMaximumMultiplier;
         ++multiplier) {
        Require(osss::IsValidMultiplier(multiplier), "every multiplier in range must validate");
    }
    Require(osss::kMaximumMultiplier >= 20, "the ceiling must reach at least 20x");

    // 20x is only reachable from a low source rate: it is the product that has
    // to be presentable, not the multiplier on its own.
    osss::FrameSelector selector(osss::kMaximumMultiplier);
    Require(
        selector.MaximumMultiplier() == osss::kMaximumMultiplier,
        "the selector must accept the ceiling it advertises");

    bool rejected = false;
    try {
        osss::FrameSelector beyond(osss::kMaximumMultiplier + 1);
        (void)beyond;
    } catch (const std::exception&) {
        rejected = true;
    }
    Require(rejected, "one past the ceiling must still be rejected by the selector");
}

void TestHistoricalTargetCounts() {
    const osss::FrameRate target{240, 1};
    constexpr auto measured = 1s;
    constexpr auto no_phase = Clock::duration::zero();
    const auto count = [&](const double source_fps, const int maximum_multiplier) {
        return RunSteadyScenario(source_fps, target, maximum_multiplier, no_phase, measured)
            .submissions;
    };

    const int sixty_to_two_x = count(60.0, 2);
    const int sixty_to_six_x = count(60.0, 6);
    const int one_twenty_to_two_x = count(120.0, 2);
    const int two_hundred_to_two_x = count(200.0, 2);
    const int three_hundred_to_six_x = count(300.0, 6);
    const int thirty_to_six_x = count(30.0, 6);

    Require(std::abs(sixty_to_two_x - 120) <= 3, "60 FPS with a 2x max must produce near 120 FPS");
    Require(std::abs(sixty_to_six_x - 240) <= 3, "60 FPS with a 6x ceiling must reach 240 FPS");
    Require(std::abs(one_twenty_to_two_x - 240) <= 3,
        "120 native FPS with a 2x max must reach 240 FPS");
    Require(std::abs(two_hundred_to_two_x - 240) <= 3,
        "200 native FPS must use only the interpolation needed to reach 240 FPS");
    Require(std::abs(three_hundred_to_six_x - 240) <= 3,
        "native FPS above the output target must remain capped by the target");
    Require(std::abs(thirty_to_six_x - 180) <= 3, "30 FPS with a 6x max must stop near 180 FPS");

    std::cout
        << "OSSS historical target counts: "
        << "60->" << sixty_to_two_x << " (2x max), "
        << "60->" << sixty_to_six_x << " (6x max), "
        << "120->" << one_twenty_to_two_x << ", "
        << "200->" << two_hundred_to_two_x << ", "
        << "300->" << three_hundred_to_six_x << ", "
        << "30->" << thirty_to_six_x << " (6x max).\n";
}

void TestOutputClock() {
    const osss::FrameRate rate{144, 1};
    osss::OutputClock clock(rate);
    const Clock::time_point start{};
    clock.Start(start);

    int deadlines = 0;
    for (Clock::time_point now = start; now <= start + 10s; now += 250us) {
        if (const auto deadline = clock.TakeLatestDue(now)) {
            ++deadlines;
            Require(deadline->skipped == 0, "250 us polling must not miss a 144 Hz deadline");
        }
    }
    Require(deadlines == 1441, "ten seconds at 144 Hz including slot zero must yield 1441 deadlines");
    const auto drift = clock.DeadlineForSlot(1440) - (start + 10s);
    Require(std::abs(drift.count()) <= 1, "absolute rational deadlines must not accumulate drift");

    osss::OutputClock late_clock(rate);
    late_clock.Start(start);
    Require(late_clock.TakeLatestDue(start).has_value(), "slot zero must be immediately due");
    const auto late = late_clock.TakeLatestDue(start + 100ms);
    Require(late.has_value() && late->slot == 14, "late polling must advance directly to newest slot");
    Require(late->skipped == 13 && late_clock.MissedDeadlineCount() == 13,
        "late polling must count skipped deadlines without returning catch-up work");

    const osss::FrameRate ntsc{60000, 1001};
    RequireNear(ntsc.AsDouble(), 59.94005994, 1.0e-8, "rational rate must retain 60000/1001");
}

void TestSteadyFractionalRates() {
    for (const double source_fps : {60.0, 50.0, 80.0}) {
        const ScenarioResult result = RunSteadyScenario(source_fps, {144, 1}, 6);
        Require(std::abs(result.measured_deadlines - 1440) <= 1,
            "target-owned clock must remain at 144 Hz for every source rate");
        Require(result.fractional_interpolations > 100,
            "fractional source/target ratios must produce fractional alphas");
        Require(result.ceiling_holds == 0,
            "60, 50, and 80 FPS must remain within the multiplier ceiling at 144 Hz");
        Require(result.underruns == 0, "steady queue-1 scenarios must not underrun after warmup");
        Require(result.missed == 0, "250 us scheduler polling must not miss target deadlines");
    }

    for (const auto phase : {0us, 1000us, 3500us, 6000us}) {
        const ScenarioResult result = RunSteadyScenario(60.0, {144, 1}, 6, phase);
        Require(std::abs(result.measured_deadlines - 1440) <= 1 &&
                result.fractional_interpolations > 100 && result.underruns == 0,
            "60-to-144 behavior must survive source/output phase offsets");
    }
}

// A source already at or above the target rate supplies a real frame for every
// output slot. Synthesising one there only softens edges and adds latency, so
// the selector must fall back to the nearer real endpoint instead.
void TestUnityPassthrough() {
    for (const auto& [source_fps, target, label] : {
             std::tuple{120.0, osss::FrameRate{120, 1}, "source equal to target"},
             std::tuple{144.0, osss::FrameRate{120, 1}, "source faster than target"},
             std::tuple{119.0, osss::FrameRate{120, 1}, "source just under target"},
             // Capture jitter routinely puts a matched source/target a few
             // percent under the target; those slots must still pass through.
             std::tuple{110.0, osss::FrameRate{120, 1}, "unity with 1.09x jitter"},
             std::tuple{105.0, osss::FrameRate{120, 1}, "unity with 1.14x jitter"},
         }) {
        const ScenarioResult result = RunSteadyScenario(source_fps, target, 6);
        Require(result.fractional_interpolations == 0,
            std::string("unity passthrough must not synthesise frames: ") + label);
        Require(result.submissions > 100,
            std::string("unity passthrough must still present real frames: ") + label);
        Require(result.underruns == 0,
            std::string("unity passthrough must not underrun: ") + label);
        Require(result.missed == 0,
            std::string("unity passthrough must not miss deadlines: ") + label);
    }

    // The guard must not swallow cases that genuinely need interpolation. The
    // 120->144 case is the binding constraint on how wide the tolerance may be:
    // it is a real display pairing frame generation exists to serve, so it must
    // stay above the threshold.
    for (const auto& [source_fps, target, label] : {
             std::tuple{60.0, osss::FrameRate{120, 1}, "60 to 120 needs 2x"},
             std::tuple{100.0, osss::FrameRate{120, 1}, "100 to 120 needs 1.2x"},
             std::tuple{120.0, osss::FrameRate{144, 1}, "120 to 144 needs 1.2x"},
             std::tuple{80.0, osss::FrameRate{120, 1}, "80 to 120 needs 1.5x"},
         }) {
        const ScenarioResult result = RunSteadyScenario(source_fps, target, 6);
        Require(result.fractional_interpolations > 100,
            std::string("rates above unity must still interpolate: ") + label);
    }
}

void TestFluctuatingSourceRate() {
    const osss::FrameRate target{144, 1};
    osss::OutputClock output(target);
    osss::SourceTimeline timeline;
    osss::FrameSelector selector(6);
    constexpr std::array source_rates{55.0, 70.0, 58.0, 66.0, 62.0, 57.0, 69.0};
    constexpr auto step = 250us;
    constexpr auto warmup = 2s;
    constexpr auto measured = 10s;
    constexpr auto capture_delay = 2ms;
    const Clock::time_point start{};
    output.Start(start);

    std::size_t cadence_index = 0;
    Clock::time_point next_media = start;
    Clock::time_point next_arrival = next_media + capture_delay;
    std::uint64_t raw_sequence = 0;
    int measured_deadlines = 0;
    int fractional = 0;
    int underruns = 0;
    int ceiling_holds = 0;

    for (Clock::time_point now = start; now <= start + warmup + measured; now += step) {
        while (next_arrival <= now) {
            timeline.Ingest(++raw_sequence, next_media, next_arrival, 1920, 1080, false);
            const double source_fps = source_rates[cadence_index % source_rates.size()];
            ++cadence_index;
            next_media += std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(1.0 / source_fps));
            next_arrival = next_media + capture_delay;
        }
        if (const auto deadline = output.TakeLatestDue(now)) {
            const auto selection = selector.Select(*deadline, target, timeline);
            if (deadline->time >= start + warmup) {
                ++measured_deadlines;
                fractional += selection.mode == osss::FrameSelectionMode::interpolate ? 1 : 0;
                underruns += selection.mode == osss::FrameSelectionMode::underrun ? 1 : 0;
                ceiling_holds += selection.mode == osss::FrameSelectionMode::ceiling_hold ? 1 : 0;
            }
        }
    }

    Require(std::abs(measured_deadlines - 1440) <= 1,
        "55-to-70 FPS fluctuation must not change the 144 Hz deadline count");
    Require(output.MissedDeadlineCount() == 0,
        "source cadence changes must not rephase or skip target deadlines");
    Require(fractional > 100 && underruns == 0 && ceiling_holds == 0,
        "fluctuating source cadence within 6x must retain bracketed fractional output");
}

void TestArrivalJitterIndependence() {
    const auto steady = RunArrivalJitterScenario({0});
    const auto jittered = RunArrivalJitterScenario({-1500, 2000, -500, 1000, 0});
    Require(steady == jittered,
        "callback arrival jitter must not change media pair or alpha selection");
}

void TestDuplicateAccounting() {
    osss::SourceTimeline timeline;
    const Clock::time_point start{};
    const auto first = timeline.Ingest(1, start, start + 2ms, 640, 360, false);
    const auto duplicate_a = timeline.Ingest(2, start + 7ms, start + 9ms, 640, 360, true);
    const auto duplicate_b = timeline.Ingest(3, start + 14ms, start + 16ms, 640, 360, true);
    const auto second = timeline.Ingest(4, start + 16666667ns, start + 19ms, 640, 360, false);

    Require(first.has_value() && second.has_value(), "changing frames must receive unique identities");
    Require(!duplicate_a && !duplicate_b, "duplicate frames must not receive unique identities");
    Require(timeline.RawFrameCount() == 4, "raw capture count must include compositor duplicates");
    Require(timeline.UniqueFrameCount() == 2, "duplicates must not advance unique source count");
    Require(timeline.DuplicateFrameCount() == 2, "duplicate count must be explicit");
    RequireNear(timeline.EstimatedSourceFps(), 60.0, 0.01,
        "duplicate timestamps must not alter unique-source cadence");
    Require(timeline.Frames().front().unique_sequence == 1 &&
            timeline.Frames().back().unique_sequence == 2,
        "duplicates must not replace interpolation endpoints");
}

void TestCeilingDistribution() {
    osss::TargetSlotGate gate;
    int allowed = 0;
    int longest_hold_run = 0;
    int hold_run = 0;
    for (int slot = 0; slot < 240; ++slot) {
        if (gate.Allow(180.0, 240.0)) {
            ++allowed;
            hold_run = 0;
        } else {
            ++hold_run;
            longest_hold_run = std::max(longest_hold_run, hold_run);
        }
    }
    Require(allowed == 180, "30 FPS at a 6x ceiling must allow exactly 180 of 240 slots");
    Require(longest_hold_run <= 1, "ceiling holds must be evenly distributed without a backlog");
}

void TestStallAndRecovery() {
    const osss::FrameRate target{144, 1};
    osss::SourceTimeline timeline;
    osss::FrameSelector selector(6);
    const Clock::time_point start{};
    timeline.Ingest(1, start, start + 2ms, 800, 600, false);
    timeline.Ingest(2, start + 16ms, start + 18ms, 800, 600, false);
    timeline.Ingest(3, start + 33ms, start + 35ms, 800, 600, false);

    osss::OutputClock::Deadline deadline{start + 200ms, 29, 0};
    const auto stalled = selector.Select(deadline, target, timeline);
    Require(stalled.mode == osss::FrameSelectionMode::stalled && stalled.alpha == 1.0F,
        "source silence must suspend interpolation and select a real frame");

    timeline.Ingest(4, start + 210ms, start + 212ms, 800, 600, false);
    deadline.time = start + 215ms;
    const auto first_recovery = selector.Select(deadline, target, timeline);
    Require(first_recovery.mode == osss::FrameSelectionMode::hold,
        "stall recovery must wait for a second healthy frame");

    timeline.Ingest(5, start + 226ms, start + 228ms, 800, 600, false);
    deadline.time = start + 232ms;
    const auto recovered = selector.Select(deadline, target, timeline);
    Require(recovered.mode != osss::FrameSelectionMode::stalled,
        "two recovery frames must leave the explicit stall state");
}

void TestColdStartUnderrunAndResize() {
    const osss::FrameRate target{144, 1};
    osss::SourceTimeline timeline;
    osss::FrameSelector selector(6);
    const Clock::time_point start{};

    timeline.Ingest(1, start, start + 2ms, 640, 360, false);
    osss::OutputClock::Deadline deadline{start + 4ms, 1, 0};
    const auto cold = selector.Select(deadline, target, timeline);
    Require(cold.mode == osss::FrameSelectionMode::cold_start &&
            cold.current_unique_sequence == 1 && cold.alpha == 1.0F,
        "cold start must select the only real frame without extrapolation");

    timeline.Ingest(2, start + 16ms, start + 18ms, 640, 360, false);
    deadline.time = start + 60ms;
    const auto underrun = selector.Select(deadline, target, timeline);
    Require(underrun.mode == osss::FrameSelectionMode::underrun &&
            underrun.current_unique_sequence == 2 && underrun.alpha == 1.0F,
        "queue underrun must select the newest real frame without extrapolation");

    const auto resized_sequence = timeline.Ingest(
        3,
        start + 70ms,
        start + 72ms,
        800,
        450,
        false);
    Require(resized_sequence.has_value() && timeline.DiscontinuityCount() == 1 &&
            timeline.Frames().size() == 1,
        "a source resize must clear incompatible history and mark a discontinuity");
    selector.Reset();
    deadline.time = start + 75ms;
    const auto resized = selector.Select(deadline, target, timeline);
    Require(resized.mode == osss::FrameSelectionMode::cold_start &&
            resized.current_unique_sequence == *resized_sequence,
        "the first post-resize frame must restart from an explicit real-frame cold state");
}

void TestMediaToIngestPercentiles() {
    osss::SourceTimeline timeline;
    const Clock::time_point start{};
    constexpr std::array delays{4ms, 8ms, 6ms, 10ms, 2ms};
    for (std::size_t index = 0; index < delays.size(); ++index) {
        const auto media = start + 1ms + 16ms * static_cast<int>(index);
        timeline.Ingest(
            index + 1,
            media,
            media + 1ms,
            640,
            360,
            false,
            media + delays[index]);
    }

    Require(timeline.MediaToIngestSampleCount() == delays.size(),
        "media-to-ingest telemetry must retain unique samples");
    RequireNear(
        timeline.MediaToIngestP50().count() * 1000.0,
        6.0,
        1.0e-9,
        "media-to-ingest p50 must use the ordered latency window");
    RequireNear(
        timeline.MediaToIngestP95().count() * 1000.0,
        10.0,
        1.0e-9,
        "media-to-ingest p95 must reach the high tail");
}

void TestAdaptiveQueueFloorAndRecovery() {
    const osss::FrameRate target{144, 1};
    const Clock::time_point start{};
    osss::SourceTimeline timeline;
    osss::FrameSelector selector(6);
    const auto source_period = 16ms;
    for (std::uint64_t sequence = 1; sequence <= 4; ++sequence) {
        const auto media = start + source_period * static_cast<int>(sequence - 1);
        timeline.Ingest(
            sequence,
            media,
            media + 2ms,
            640,
            360,
            false,
            media + 2ms);
    }

    osss::OutputClock::Deadline deadline{start + 100ms, 14, 0};
    const auto underrun = selector.Select(deadline, target, timeline);
    Require(underrun.mode == osss::FrameSelectionMode::underrun,
        "a missing future endpoint must be classified as an underrun");
    Require(underrun.queue_delay.count() > 24.0e-3,
        "an underrun must grow the queue beyond the 8 ms floor");

    const auto after_underrun_delay = underrun.queue_delay.count();
    timeline.Ingest(
        5,
        start + 64ms,
        start + 66ms,
        640,
        360,
        false,
        start + 66ms);
    deadline.time = start + 80ms;
    const auto recovered = selector.Select(deadline, target, timeline);
    Require(recovered.mode == osss::FrameSelectionMode::interpolate,
        "a later endpoint must restore bracketed interpolation");
    Require(recovered.queue_delay.count() < after_underrun_delay,
        "clean source frames must slowly decay the adaptive queue");

    osss::FrameSelector high_floor(6, 16ms);
    const auto high_floor_selection = high_floor.Select(deadline, target, timeline);
    Require(high_floor_selection.queue_delay.count() >= 32.0e-3,
        "the user buffer floor must be reflected in queue delay");
}

void TestSelectionCounters() {
    osss::FrameSelectionCounters cumulative;
    cumulative.Record(osss::FrameSelectionMode::interpolate);
    cumulative.Record(osss::FrameSelectionMode::interpolate);
    cumulative.Record(osss::FrameSelectionMode::underrun);
    cumulative.Record(osss::FrameSelectionMode::ceiling_hold);
    osss::FrameSelectionCounters previous;
    previous.Record(osss::FrameSelectionMode::interpolate);
    const auto interval = osss::Difference(cumulative, previous);
    Require(interval.interpolate == 1 && interval.underrun == 1 && interval.ceiling_hold == 1,
        "selection counters must expose interval deltas by mode");
    Require(cumulative.interpolate == 2 && cumulative.underrun == 1,
        "selection counters must retain cumulative mode totals");
}

void TestEvenCeilingCadence() {
    osss::TargetSlotGate gate;
    int allowed = 0;
    int consecutive_allowed = 0;
    int maximum_consecutive_allowed = 0;
    for (int slot = 0; slot < 240; ++slot) {
        if (gate.AllowEven(180.0, 240.0)) {
            ++allowed;
            ++consecutive_allowed;
            maximum_consecutive_allowed = std::max(
                maximum_consecutive_allowed,
                consecutive_allowed);
        } else {
            consecutive_allowed = 0;
        }
    }
    Require(allowed == 120,
        "even ceiling pacing must present every second slot for a 180/240 ceiling");
    Require(maximum_consecutive_allowed == 1,
        "even ceiling pacing must not burst two submissions together");
}

// The generation toggle, driven the way the presentation loop drives it: flipped
// between output slots, mid-run, without resetting anything else.
//
// What "off" has to mean is the whole test. Not fewer generated frames -- none.
// Not a paused clock -- the same clock, declining the slots the source cannot
// fill. And it has to be reversible on the next slot, because the entire reason
// this exists is A/B comparison on one scene.
void TestGenerationToggle() {
    const osss::FrameRate target{240, 1};
    osss::OutputClock output(target);
    osss::SourceTimeline timeline;
    osss::FrameSelector selector(6);
    constexpr auto step = 250us;
    constexpr auto warmup = 2s;
    constexpr auto phase = 1s;
    constexpr auto capture_delay = 2ms;
    const Clock::time_point start{};
    output.Start(start);

    const Clock::duration source_period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / 60.0));
    Clock::time_point next_media = start;
    Clock::time_point next_arrival = next_media + capture_delay;
    std::uint64_t raw_sequence = 0;

    struct Phase {
        std::uint64_t submissions = 0;
        std::uint64_t interpolations = 0;
        std::uint64_t ceiling_holds = 0;
        std::uint64_t underruns = 0;
    };
    Phase generating_before{};
    Phase paused{};
    Phase generating_after{};

    for (Clock::time_point now = start; now <= start + warmup + phase * 3; now += step) {
        while (next_arrival <= now) {
            timeline.Ingest(++raw_sequence, next_media, next_arrival, 1920, 1080, false);
            next_media += source_period;
            next_arrival = next_media + capture_delay;
        }

        // On for the warmup and the first measured second, off for the second,
        // on again for the third.
        const bool generation_on =
            now < start + warmup + phase || now >= start + warmup + phase * 2;
        selector.SetGenerationEnabled(generation_on);
        Require(selector.GenerationEnabled() == generation_on, "the setter must stick");

        const auto deadline = output.TakeLatestDue(now);
        if (!deadline) {
            continue;
        }
        const osss::FrameSelection selection = selector.Select(*deadline, target, timeline);

        Phase* bucket = nullptr;
        if (now >= start + warmup + phase * 2) {
            bucket = &generating_after;
        } else if (now >= start + warmup + phase) {
            bucket = &paused;
            // Everything that reaches the display while paused must be a real
            // frame at a whole alpha. A blend at alpha 0.999 is still a blend,
            // and it is exactly what an A/B must not be quietly showing.
            Require(
                selection.mode != osss::FrameSelectionMode::interpolate,
                "a paused selector must never interpolate");
            if (selection.submit) {
                Require(selection.alpha == 1.0F, "a paused selection must present a whole frame");
            }
        } else if (now >= start + warmup) {
            bucket = &generating_before;
        }
        if (!bucket) {
            continue;
        }
        if (selection.submit) {
            ++bucket->submissions;
        }
        if (selection.mode == osss::FrameSelectionMode::interpolate) {
            ++bucket->interpolations;
        }
        if (selection.mode == osss::FrameSelectionMode::ceiling_hold) {
            ++bucket->ceiling_holds;
        }
        if (selection.mode == osss::FrameSelectionMode::underrun) {
            ++bucket->underruns;
        }
    }

    // Generating: a 60 FPS source into a 240 Hz target fills every slot.
    const auto require_generating = [](const Phase& generating, const char* const label) {
        Require(generating.submissions >= 230 && generating.submissions <= 245,
            std::string("generation on must fill the target rate: ") + label);
        Require(generating.interpolations > 100,
            std::string("generation on must actually interpolate: ") + label);
    };
    require_generating(generating_before, "before the pause");
    require_generating(generating_after, "after the pause");

    // Paused: presents fall to the source rate, and the slots in between are
    // declined rather than filled with a repeat.
    Require(paused.interpolations == 0, "a paused selector must generate nothing at all");
    Require(paused.submissions >= 50 && paused.submissions <= 70,
        "a paused selector must present at about the source rate");
    Require(paused.ceiling_holds > 150, "the slots the source cannot fill must be declined");
    Require(paused.underruns == 0, "pausing must not underrun");
}

// The toggle is a user decision, and a capture discontinuity is not a reason to
// overturn it. Reset clears pacing state; it must leave this alone, or a stall
// would silently start generating again behind the user's back.
void TestGenerationToggleSurvivesReset() {
    osss::FrameSelector selector(6);
    Require(selector.GenerationEnabled(), "generation is on by default");
    selector.SetGenerationEnabled(false);
    selector.Reset();
    Require(!selector.GenerationEnabled(), "Reset must not turn generation back on");
    selector.SetGenerationEnabled(true);
    selector.Reset();
    Require(selector.GenerationEnabled(), "Reset must not turn generation off either");
}

void TestFrameSelectorEvenCeiling() {
    const auto result = RunSteadyScenario(
        30.0,
        {240, 1},
        6,
        Clock::duration::zero(),
        1s,
        osss::FrameSelector::CeilingPacing::even);
    Require(std::abs(result.submissions - 120) <= 2,
        "the production even selector must submit a stable 120 FPS from a 30 FPS source");
    Require(result.ceiling_holds > 100,
        "the even selector must expose its ceiling holds to telemetry");
}

} // namespace

void TestVblankPhaseAlignment() {
    using namespace std::chrono_literals;
    constexpr auto refresh = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / 120.0));

    // Commensurate: a 60 FPS target on a 120 Hz panel puts a deadline on every
    // second vblank, so a phase exists that satisfies all of them.
    {
        osss::OutputClock clock(osss::FrameRate{60, 1});
        const Clock::time_point start{};
        clock.Start(start);
        const auto rate_before = clock.Rate();

        // A vblank deliberately out of phase with the grid by a third of a slot.
        const auto slot = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / 60.0));
        const auto lead = std::chrono::duration_cast<Clock::duration>(1500us);
        auto vblank = start + slot / 3 + lead;

        double first_error_seconds = 0.0;
        for (int iteration = 0; iteration < 200; ++iteration) {
            Require(clock.PhaseAlignToVblank(vblank, refresh, lead) ||
                    iteration > 0,
                "an out-of-phase commensurate grid must accept a first correction");
            // How far slot zero's deadline, plus its lead, sits off the vblank
            // raster. Measured modulo the refresh period for the same reason
            // the servo corrects modulo it.
            const double offset = std::chrono::duration<double>(
                (clock.DeadlineForSlot(0) + lead) - vblank).count();
            const double refresh_seconds = std::chrono::duration<double>(refresh).count();
            double phase = std::fmod(offset, refresh_seconds);
            if (phase < 0.0) {
                phase += refresh_seconds;
            }
            if (phase > refresh_seconds * 0.5) {
                phase -= refresh_seconds;
            }
            if (iteration == 0) {
                first_error_seconds = std::abs(phase);
            }
            if (iteration == 199) {
                Require(std::abs(phase) < first_error_seconds,
                    "repeated corrections must reduce the phase error");
                Require(std::abs(phase) < 0.0005,
                    "the phase servo must converge to well under a millisecond");
            }
            vblank += refresh;
        }

        Require(clock.Rate().numerator == rate_before.numerator &&
                clock.Rate().denominator == rate_before.denominator,
            "phase alignment must never change the target rate");
        Require(clock.PhaseCorrectionCount() > 0,
            "a converged alignment must have recorded its corrections");
    }

    // Incommensurate: 120 FPS against a 144 Hz panel. No fixed phase satisfies
    // every vblank, so the clock must decline rather than pretend.
    {
        constexpr auto refresh_144 = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / 144.0));
        osss::OutputClock clock(osss::FrameRate{120, 1});
        const Clock::time_point start{};
        clock.Start(start);
        const auto epoch_before = clock.DeadlineForSlot(0);
        Require(!clock.PhaseAlignToVblank(start + 3ms, refresh_144, 1500us),
            "an incommensurate target and refresh pair must not be aligned");
        Require(clock.DeadlineForSlot(0) == epoch_before,
            "a declined alignment must leave the grid untouched");
        Require(clock.PhaseCorrectionCount() == 0,
            "a declined alignment must not count as a correction");
    }

    // A correction is capped, so one wild sample cannot displace the grid.
    {
        osss::OutputClock clock(osss::FrameRate{60, 1});
        const Clock::time_point start{};
        clock.Start(start);
        const auto epoch_before = clock.DeadlineForSlot(0);
        static_cast<void>(clock.PhaseAlignToVblank(start + 8ms, refresh, 0ms));
        const auto moved = clock.DeadlineForSlot(0) - epoch_before;
        Require(moved <= 500us && moved >= -500us,
            "a single phase correction must stay inside its cap");
    }

    // Slot issuance stays monotonic across an alignment: moving the epoch must
    // never let an already-taken slot be handed out again.
    {
        osss::OutputClock clock(osss::FrameRate{60, 1});
        const Clock::time_point start{};
        clock.Start(start);
        const auto first = clock.TakeLatestDue(start + 100ms);
        Require(first.has_value(), "a due slot must be available");
        static_cast<void>(clock.PhaseAlignToVblank(start + 100ms + 4ms, refresh, 0ms));
        const auto second = clock.TakeLatestDue(start + 200ms);
        Require(second.has_value() && second->slot > first->slot,
            "phase alignment must not rewind slot issuance");
    }
}

void TestPacingMonitor() {
    using namespace std::chrono_literals;
    constexpr double target = 120.0;
    const auto period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / target));
    const Clock::time_point start{};

    // One present is not an interval.
    {
        osss::PacingMonitor monitor;
        monitor.RecordPresent(start);
        const auto report = monitor.Summarize(target);
        Require(report.sample_count == 0, "a single present must yield no intervals");
        Require(report.on_time_fraction == 0.0,
            "an empty pacing report must not claim frames were on time");
    }

    // Perfect spacing: everything on time, no deviation.
    {
        osss::PacingMonitor monitor;
        for (int index = 0; index <= 200; ++index) {
            monitor.RecordPresent(start + period * index);
        }
        const auto report = monitor.Summarize(target);
        Require(report.sample_count == 200, "every interval must be counted");
        RequireNear(report.p50_milliseconds, 1000.0 / target, 0.05,
            "even spacing must report the target period at p50");
        Require(report.on_time_fraction > 0.999,
            "even spacing must be fully on time");
        Require(report.mean_absolute_error_milliseconds < 0.05,
            "even spacing must have almost no mean absolute error");
    }

    // The case this whole class exists for: the correct number of frames, and
    // badly uneven spacing. Alternating half and one-and-a-half periods gives
    // exactly the target rate on average, so every FPS counter in the codebase
    // reads correct -- and none of them can see the judder.
    {
        osss::PacingMonitor monitor;
        Clock::time_point when = start;
        monitor.RecordPresent(when);
        for (int index = 0; index < 200; ++index) {
            when += (index % 2 == 0) ? period / 2 : period + period / 2;
            monitor.RecordPresent(when);
        }
        const auto report = monitor.Summarize(target);
        const double mean_interval_ms =
            std::chrono::duration<double, std::milli>(when - start).count() /
            static_cast<double>(report.sample_count);
        RequireNear(mean_interval_ms, 1000.0 / target, 0.05,
            "the alternating pattern must average exactly the target period");
        Require(report.on_time_fraction < 0.01,
            "alternating half and one-and-a-half periods must read as off-time");
        Require(report.p95_milliseconds > report.p50_milliseconds * 2.0,
            "an alternating pattern must separate p50 from p95");
        Require(report.mean_absolute_error_milliseconds > 1.0,
            "uneven spacing must show a large mean absolute error");
    }

    // Reset drops the baseline, so the gap it spans is never charged.
    {
        osss::PacingMonitor monitor;
        monitor.RecordPresent(start);
        monitor.RecordPresent(start + period);
        monitor.Reset();
        Require(monitor.SampleCount() == 0, "Reset must clear recorded intervals");
        monitor.RecordPresent(start + 5s);
        monitor.RecordPresent(start + 5s + period);
        const auto report = monitor.Summarize(target);
        Require(report.sample_count == 1,
            "the interval spanning a reset must not be recorded");
        Require(report.maximum_milliseconds < 100.0,
            "a five-second hide must not appear as a pacing outlier");
    }

    // The window is bounded, so a long session cannot grow without limit and
    // old behaviour ages out of the report.
    {
        osss::PacingMonitor monitor(8);
        for (int index = 0; index <= 100; ++index) {
            monitor.RecordPresent(start + period * index);
        }
        Require(monitor.SampleCount() == 8, "the sample window must stay bounded");
    }

    // A non-monotonic timestamp must re-baseline rather than record a negative
    // interval, which would drag the percentiles below target and read as
    // suspiciously good pacing.
    {
        osss::PacingMonitor monitor;
        monitor.RecordPresent(start + 1s);
        monitor.RecordPresent(start);
        monitor.RecordPresent(start + period);
        const auto report = monitor.Summarize(target);
        Require(report.sample_count == 1, "a backwards timestamp must only re-baseline");
        Require(report.p50_milliseconds > 0.0, "no interval may be negative");
    }
}

// A steady 60 FPS source feeding two selectors in lockstep from the same
// timeline. `paced` selects slot k when slot k is due; `ahead` has one slot of
// lookahead and selects slot k+1 at that same instant, which is what the queued
// pacing mode does. Returns (slot, previous, current, alpha) records for both.
struct LookaheadRecord {
    std::uint64_t slot = 0;
    osss::FrameSelectionMode mode = osss::FrameSelectionMode::no_frame;
    std::uint64_t previous = 0;
    std::uint64_t current = 0;
    int alpha_micro = 0;
    double queue_delay_ms = 0.0;
};

void TestLookaheadSelectsOneSlotEarly() {
    const osss::FrameRate target{240, 1};
    osss::OutputClock output(target);
    osss::SourceTimeline timeline;
    osss::FrameSelector paced(6);
    osss::FrameSelector ahead(6);
    const auto slot_period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / target.AsDouble()));
    ahead.SetLookahead(slot_period);
    Require(ahead.Lookahead() == slot_period, "lookahead must read back as set");
    Require(paced.Lookahead() == Clock::duration::zero(), "lookahead defaults to zero");

    constexpr auto source_period = 16666667ns;
    constexpr auto capture_delay = 2ms;
    constexpr auto step = 250us;
    const Clock::time_point start{};
    output.Start(start);
    Clock::time_point next_media = start;
    Clock::time_point next_arrival = next_media + capture_delay;
    std::uint64_t raw_sequence = 0;
    std::vector<LookaheadRecord> paced_records;
    std::vector<LookaheadRecord> ahead_records;

    for (Clock::time_point now = start; now <= start + 3s; now += step) {
        while (next_arrival <= now) {
            timeline.Ingest(++raw_sequence, next_media, next_arrival, 1280, 720, false);
            next_media += source_period;
            next_arrival = next_media + capture_delay;
        }
        if (const auto deadline = output.TakeLatestDue(now)) {
            const auto p = paced.Select(*deadline, target, timeline);
            osss::OutputClock::Deadline next{output.DeadlineForSlot(deadline->slot + 1),
                deadline->slot + 1, 0};
            const auto a = ahead.Select(next, target, timeline);
            if (deadline->time >= start + 1s) {
                paced_records.push_back({deadline->slot, p.mode, p.previous_unique_sequence,
                    p.current_unique_sequence,
                    static_cast<int>(std::lround(p.alpha * 1'000'000.0F)),
                    p.queue_delay.count() * 1000.0});
                ahead_records.push_back({next.slot, a.mode, a.previous_unique_sequence,
                    a.current_unique_sequence,
                    static_cast<int>(std::lround(a.alpha * 1'000'000.0F)),
                    a.queue_delay.count() * 1000.0});
            }
        }
    }

    Require(paced_records.size() > 400 && paced_records.size() == ahead_records.size(),
        "both selectors must have been consulted once per slot");

    // The queue delay is longer by exactly the lookahead, and every
    // ahead-of-time selection is a bracketed interpolation -- no underrun from
    // asking one slot early, because the media clock moved back to pay for it.
    for (std::size_t index = 0; index < ahead_records.size(); ++index) {
        Require(ahead_records[index].mode == osss::FrameSelectionMode::interpolate,
            "lookahead selection one slot early must still be bracketed");
        Require(paced_records[index].mode == osss::FrameSelectionMode::interpolate,
            "the paced reference must be bracketed too");
        RequireNear(
            ahead_records[index].queue_delay_ms - paced_records[index].queue_delay_ms,
            std::chrono::duration<double, std::milli>(slot_period).count(),
            0.01,
            "queue delay must grow by exactly the lookahead");
    }

    // The load-bearing property: the frame prepared ahead for slot k+1 shows
    // the media instant paced would have shown at slot k. That is the one
    // target slot of latency the queued mode costs, and it is all it costs --
    // the pair and alpha match exactly, so nothing else about selection moved.
    for (std::size_t index = 0; index < ahead_records.size(); ++index) {
        Require(ahead_records[index].slot == paced_records[index].slot + 1,
            "ahead record must be for the following slot");
        Require(ahead_records[index].previous == paced_records[index].previous &&
                ahead_records[index].current == paced_records[index].current,
            "ahead selection for slot k+1 must use the pair paced used for slot k");
        Require(std::abs(ahead_records[index].alpha_micro - paced_records[index].alpha_micro) <= 2,
            "ahead selection for slot k+1 must use the alpha paced used for slot k");
    }
}

// SelectNow is the unpaced loop's question -- "what do I show now?" -- and it
// must not touch the ceiling gate. Two things follow: it never returns a
// ceiling hold itself, and interleaving it with Select leaves Select's own
// hold pattern exactly as it would have been.
void TestSelectNowBypassesCeilingAndLeavesGateAlone() {
    const osss::FrameRate target{240, 1};
    constexpr auto source_period = 16666667ns;
    constexpr auto capture_delay = 2ms;
    constexpr auto step = 250us;
    const Clock::time_point start{};

    const auto run = [&](const bool interleave_select_now) {
        osss::OutputClock output(target);
        osss::SourceTimeline timeline;
        // Multiplier 2 against a 4x requirement: half the slots are ceiling holds.
        osss::FrameSelector selector(2);
        output.Start(start);
        Clock::time_point next_media = start;
        Clock::time_point next_arrival = next_media + capture_delay;
        std::uint64_t raw_sequence = 0;
        std::vector<osss::FrameSelectionMode> paced_modes;
        std::size_t select_now_calls = 0;
        std::size_t select_now_holds = 0;
        std::size_t select_now_interpolations = 0;
        for (Clock::time_point now = start; now <= start + 3s; now += step) {
            while (next_arrival <= now) {
                timeline.Ingest(++raw_sequence, next_media, next_arrival, 1280, 720, false);
                next_media += source_period;
                next_arrival = next_media + capture_delay;
            }
            if (interleave_select_now && now >= start + 1s) {
                const auto immediate = selector.SelectNow(now, target, timeline);
                ++select_now_calls;
                if (immediate.mode == osss::FrameSelectionMode::ceiling_hold) {
                    ++select_now_holds;
                }
                if (immediate.mode == osss::FrameSelectionMode::interpolate) {
                    Require(immediate.submit, "an interpolation must be submittable");
                    Require(immediate.deadline.time == now && immediate.deadline.slot == 0,
                        "SelectNow reports now as its deadline and no slot");
                    ++select_now_interpolations;
                }
            }
            if (const auto deadline = output.TakeLatestDue(now)) {
                const auto selection = selector.Select(*deadline, target, timeline);
                if (deadline->time >= start + 1s) {
                    paced_modes.push_back(selection.mode);
                }
            }
        }
        if (interleave_select_now) {
            Require(select_now_calls > 1000, "SelectNow must have been exercised");
            Require(select_now_holds == 0, "SelectNow must never return a ceiling hold");
            Require(select_now_interpolations > select_now_calls * 9 / 10,
                "SelectNow on a steady source must interpolate nearly always");
        }
        return paced_modes;
    };

    const auto reference = run(false);
    const auto interleaved = run(true);
    std::size_t holds = 0;
    for (const auto mode : reference) {
        if (mode == osss::FrameSelectionMode::ceiling_hold) {
            ++holds;
        }
    }
    Require(holds > reference.size() / 3, "the reference must actually be ceiling-bound");
    Require(reference == interleaved,
        "SelectNow must not advance the ceiling gate: Select's hold pattern must be identical");
}

// Unpaced with generation off must still be a real-frame-only path.
void TestSelectNowGenerationOff() {
    const osss::FrameRate target{240, 1};
    osss::SourceTimeline timeline;
    osss::FrameSelector selector(6);
    const Clock::time_point start{};
    timeline.Ingest(1, start, start + 2ms, 640, 360, false);
    timeline.Ingest(2, start + 16ms, start + 18ms, 640, 360, false);
    timeline.Ingest(3, start + 33ms, start + 35ms, 640, 360, false);
    selector.SetGenerationEnabled(false);
    const auto selection = selector.SelectNow(start + 40ms, target, timeline);
    Require(selection.mode == osss::FrameSelectionMode::real_frame &&
            selection.current_unique_sequence == 3 && selection.alpha == 1.0F && selection.submit,
        "generation off must select the newest real frame from SelectNow");
}

int main() {
    try {
        TestRateLimits();
        TestHistoricalTargetCounts();
        TestHighMultiplierCeiling();
        TestOutputClock();
        TestSteadyFractionalRates();
        TestUnityPassthrough();
        TestFluctuatingSourceRate();
        TestArrivalJitterIndependence();
        TestDuplicateAccounting();
        TestCeilingDistribution();
        TestStallAndRecovery();
        TestColdStartUnderrunAndResize();
        TestMediaToIngestPercentiles();
        TestAdaptiveQueueFloorAndRecovery();
        TestSelectionCounters();
        TestEvenCeilingCadence();
        TestFrameSelectorEvenCeiling();
        TestGenerationToggle();
        TestGenerationToggleSurvivesReset();
        TestVblankPhaseAlignment();
        TestPacingMonitor();
        TestLookaheadSelectsOneSlotEarly();
        TestSelectNowBypassesCeilingAndLeavesGateAlone();
        TestSelectNowGenerationOff();
        std::cout
            << "OSSS adaptive scheduler passed: fixed rational deadlines, fractional pair selection, "
               "jitter independence, duplicate isolation, bounded ceiling slots, explicit fallbacks, "
               "resize discontinuity, and stall recovery.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
    }
    return EXIT_FAILURE;
}
