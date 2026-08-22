#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace osss {

struct FrameRate {
    std::uint64_t numerator = 60;
    std::uint64_t denominator = 1;

    [[nodiscard]] static FrameRate FromFps(double frames_per_second);
    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] double AsDouble() const noexcept;
};

class OutputClock {
public:
    using Clock = std::chrono::steady_clock;

    struct Deadline {
        Clock::time_point time{};
        std::uint64_t slot = 0;
        std::uint64_t skipped = 0;
    };

    explicit OutputClock(FrameRate rate = {});

    void SetRate(FrameRate rate);
    void Start(Clock::time_point epoch) noexcept;
    void Reset() noexcept;

    [[nodiscard]] bool Started() const noexcept;
    [[nodiscard]] FrameRate Rate() const noexcept;
    [[nodiscard]] Clock::time_point NextDeadline() const noexcept;
    // The slot NextDeadline() belongs to: the first one TakeLatestDue has not
    // yet issued. A render-ahead loop selects and draws this slot before it is
    // due, then presents it when TakeLatestDue hands it over.
    [[nodiscard]] std::uint64_t NextSlot() const noexcept;
    [[nodiscard]] Clock::time_point DeadlineForSlot(std::uint64_t slot) const noexcept;
    [[nodiscard]] std::optional<Deadline> TakeLatestDue(Clock::time_point now) noexcept;
    [[nodiscard]] std::uint64_t MissedDeadlineCount() const noexcept;
    void RecordMissedSubmission(std::uint64_t count = 1) noexcept;

    // Phase-align the deadline grid to the display's vblank raster.
    //
    // This is the one thing allowed to move the epoch after Start, and it is
    // worth being precise about why it does not contradict "the target clock
    // never re-phases from the source". It re-phases from the *display*, which
    // is the clock the frames are ultimately shown on. The source is still
    // never consulted.
    //
    // Why it is needed at all: under sync interval one every present is held to
    // a vblank, so a deadline that lands just after one waits an entire refresh
    // period before it is shown. With an unaligned epoch that offset is whatever
    // instant the process happened to start at, and it is as likely to be the
    // worst value as the best. Aiming each deadline `lead` ahead of a vblank
    // leaves exactly enough time to render and present into that same vblank.
    //
    // Only the phase moves; `rate_` is never touched, so the long-run output
    // rate is exactly the rational target it always was. The correction is a
    // fraction of the measured error and is capped per call, which makes this a
    // servo rather than a jump: a single bad DXGI sample cannot displace the
    // grid, and no already-issued slot can be re-issued because `next_slot_` is
    // monotonic regardless of where the epoch sits.
    //
    // Returns false without touching anything when the grids are incommensurate
    // -- a 120 FPS target on a 144 Hz panel cannot be aligned to every vblank,
    // because no fixed phase exists that satisfies all of them. That case is
    // what tearing mode is for, and silently "aligning" it would be a lie.
    bool PhaseAlignToVblank(
        Clock::time_point vblank,
        Clock::duration refresh_period,
        Clock::duration lead) noexcept;
    [[nodiscard]] std::uint64_t PhaseCorrectionCount() const noexcept;
    [[nodiscard]] Clock::duration LastPhaseCorrection() const noexcept;

private:
    // Fraction of the measured phase error applied per correction. Low enough
    // that DXGI sample noise averages out over several presents rather than
    // steering the grid, high enough to converge in well under a second.
    static constexpr double kPhaseCorrectionGain = 0.125;
    // Hard cap on one correction. A correction larger than this is a sign the
    // sample is wrong -- a mode change, a monitor swap, a disjoint statistics
    // window -- and walking there over several frames is always safe, whereas
    // jumping there produces a visible hitch.
    static constexpr auto kMaximumPhaseCorrection = std::chrono::microseconds(500);
    // How far the slot period may sit from an exact multiple (or divisor) of the
    // refresh period and still be considered alignable. One percent covers the
    // rational-vs-integer difference between 59.94 and 60 without admitting
    // genuinely incommensurate pairs such as 120 against 144.
    static constexpr double kCommensurateTolerance = 0.01;

    FrameRate rate_{};
    Clock::time_point epoch_{};
    std::uint64_t next_slot_ = 0;
    std::uint64_t missed_deadlines_ = 0;
    std::uint64_t phase_corrections_ = 0;
    Clock::duration last_phase_correction_{};
    bool started_ = false;
};

struct SourceFrameSample {
    std::uint64_t raw_sequence = 0;
    std::uint64_t unique_sequence = 0;
    OutputClock::Clock::time_point media_time{};
    OutputClock::Clock::time_point arrival_time{};
    OutputClock::Clock::time_point ingest_time{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

class SourceTimeline {
public:
    using Clock = OutputClock::Clock;

    explicit SourceTimeline(std::size_t maximum_history = 8);

    std::optional<std::uint64_t> Ingest(
        std::uint64_t raw_sequence,
        Clock::time_point media_time,
        Clock::time_point arrival_time,
        std::uint32_t width,
        std::uint32_t height,
        bool duplicate,
        Clock::time_point ingest_time = {});
    void ResetHistory() noexcept;

    [[nodiscard]] const std::deque<SourceFrameSample>& Frames() const noexcept;
    [[nodiscard]] std::uint64_t RawFrameCount() const noexcept;
    [[nodiscard]] std::uint64_t UniqueFrameCount() const noexcept;
    [[nodiscard]] std::uint64_t DuplicateFrameCount() const noexcept;
    [[nodiscard]] std::uint64_t DiscontinuityCount() const noexcept;
    [[nodiscard]] double EstimatedSourceFps() const noexcept;
    [[nodiscard]] std::chrono::duration<double> EstimatedSourcePeriod() const noexcept;
    [[nodiscard]] std::chrono::duration<double> CaptureDelay() const noexcept;
    [[nodiscard]] std::chrono::duration<double> MediaToIngestP50() const noexcept;
    [[nodiscard]] std::chrono::duration<double> MediaToIngestP95() const noexcept;
    [[nodiscard]] std::size_t MediaToIngestSampleCount() const noexcept;
    [[nodiscard]] bool IsStalled(Clock::time_point now) const noexcept;

private:
    static constexpr double kDefaultPeriodSeconds = 1.0 / 60.0;
    static constexpr double kMinimumPeriodSeconds = 1.0 / 500.0;
    static constexpr double kMaximumPeriodSeconds = 0.250;
    static constexpr double kStallPeriodSeconds = 0.100;
    static constexpr double kPeriodSampleWeight = 0.15;
    static constexpr double kCaptureDelaySampleWeight = 0.10;
    static constexpr std::size_t kLatencySampleWindow = 256;

    std::size_t maximum_history_ = 8;
    std::deque<SourceFrameSample> frames_;
    std::uint64_t raw_frame_count_ = 0;
    std::uint64_t unique_frame_count_ = 0;
    std::uint64_t duplicate_frame_count_ = 0;
    std::uint64_t discontinuity_count_ = 0;
    double source_period_seconds_ = kDefaultPeriodSeconds;
    double capture_delay_seconds_ = 0.0;
    bool has_period_sample_ = false;
    bool has_capture_delay_sample_ = false;
    std::deque<double> media_to_ingest_seconds_;
};

// Measures how evenly presents actually landed.
//
// Every other rate in this codebase answers "how many frames", and none of them
// answers "were they evenly spaced". Those are independent: 144 submitted frames
// per second reads identically whether the intervals were all 6.94 ms or half
// were 3 ms and half 11 ms. The second case is what judder *is*, and until this
// existed nothing in OSSS could distinguish them -- which is precisely how
// "pacing is atrocious" could coexist with a telemetry line that looked correct.
//
// Fed from the presentation path with the timestamp of each submitted present,
// so it measures the interval OSSS is responsible for. It cannot see the
// display: a present is when the frame was handed over, not when it was scanned
// out. `confirmed_fps` remains the only DXGI-side evidence, and neither is
// physical proof. Deliberately dependency-free and main-thread-only, like the
// rest of this header.
class PacingMonitor {
public:
    using Clock = OutputClock::Clock;

    struct Report {
        double p50_milliseconds = 0.0;
        double p95_milliseconds = 0.0;
        double maximum_milliseconds = 0.0;
        // Fraction of intervals within kOnTimeTolerance of the target period,
        // in [0, 1]. This is the number to read first: it collapses the
        // distribution into "how often was a frame where it should have been".
        double on_time_fraction = 0.0;
        // Mean absolute deviation from the target period. Distinguishes a few
        // large misses from a permanent slight wobble, which the percentiles
        // alone can read the same way.
        double mean_absolute_error_milliseconds = 0.0;
        std::size_t sample_count = 0;
    };

    // An interval counts as on time when it is within this fraction of the
    // target period. 15% of 6.94 ms is about 1 ms, which is roughly the
    // threshold at which uneven spacing stops being visible on a fast panel.
    static constexpr double kOnTimeTolerance = 0.15;

    explicit PacingMonitor(std::size_t window = 512);

    // Timestamps must be non-decreasing. The first call after construction or
    // Reset only establishes the baseline: one present is not an interval.
    void RecordPresent(Clock::time_point when) noexcept;
    // Drops the history and the baseline. Call whenever a gap in presenting is
    // expected and should not be charged as a pacing failure -- the overlay
    // hiding, a capture restart -- because the interval spanning the gap is not
    // a pacing measurement, it is the gap.
    void Reset() noexcept;

    [[nodiscard]] Report Summarize(double target_fps) const noexcept;
    [[nodiscard]] std::size_t SampleCount() const noexcept;

private:
    std::size_t window_ = 512;
    std::deque<double> intervals_seconds_;
    Clock::time_point last_present_{};
    bool has_last_present_ = false;
};

class TargetSlotGate {
public:
    void Reset() noexcept;
    [[nodiscard]] bool Allow(double allowed_rate, double target_rate) noexcept;
    [[nodiscard]] bool AllowEven(double allowed_rate, double target_rate) noexcept;

private:
    double credit_ = 0.0;
    bool initialized_ = false;
    std::uint64_t even_slot_ = 0;
    std::uint64_t even_interval_ = 0;
    bool even_initialized_ = false;
};

enum class FrameSelectionMode {
    no_frame,
    cold_start,
    interpolate,
    real_frame,
    hold,
    ceiling_hold,
    underrun,
    stalled,
};

struct FrameSelectionCounters {
    std::uint64_t no_frame = 0;
    std::uint64_t cold_start = 0;
    std::uint64_t interpolate = 0;
    std::uint64_t real_frame = 0;
    std::uint64_t hold = 0;
    std::uint64_t ceiling_hold = 0;
    std::uint64_t underrun = 0;
    std::uint64_t stalled = 0;

    void Record(const FrameSelectionMode mode) noexcept {
        switch (mode) {
        case FrameSelectionMode::no_frame:
            ++no_frame;
            break;
        case FrameSelectionMode::cold_start:
            ++cold_start;
            break;
        case FrameSelectionMode::interpolate:
            ++interpolate;
            break;
        case FrameSelectionMode::real_frame:
            ++real_frame;
            break;
        case FrameSelectionMode::hold:
            ++hold;
            break;
        case FrameSelectionMode::ceiling_hold:
            ++ceiling_hold;
            break;
        case FrameSelectionMode::underrun:
            ++underrun;
            break;
        case FrameSelectionMode::stalled:
            ++stalled;
            break;
        }
    }
};

[[nodiscard]] inline FrameSelectionCounters Difference(
    const FrameSelectionCounters& current,
    const FrameSelectionCounters& previous) noexcept {
    const auto delta = [](const std::uint64_t now, const std::uint64_t before) {
        return now >= before ? now - before : now;
    };
    return FrameSelectionCounters{
        delta(current.no_frame, previous.no_frame),
        delta(current.cold_start, previous.cold_start),
        delta(current.interpolate, previous.interpolate),
        delta(current.real_frame, previous.real_frame),
        delta(current.hold, previous.hold),
        delta(current.ceiling_hold, previous.ceiling_hold),
        delta(current.underrun, previous.underrun),
        delta(current.stalled, previous.stalled),
    };
}

struct FrameSelection {
    FrameSelectionMode mode = FrameSelectionMode::no_frame;
    OutputClock::Deadline deadline{};
    OutputClock::Clock::time_point media_time{};
    std::uint64_t previous_unique_sequence = 0;
    std::uint64_t current_unique_sequence = 0;
    float alpha = 1.0F;
    bool submit = false;
    double required_multiplier = 0.0;
    double allowed_multiplier = 0.0;
    std::size_t queue_occupancy = 0;
    std::chrono::duration<double> queue_delay{};
};

class FrameSelector {
public:
    using Clock = OutputClock::Clock;

    enum class CeilingPacing {
        spread,
        even,
    };

    // 8 ms, and it is not padding: measured against
    // TestFluctuatingSourceRate -- a source alternating between 55 and 70 FPS
    // into a 144 Hz target -- 7 ms is the smallest floor that produces no
    // underruns at all, and 6 ms produces them. The margin here is about one
    // millisecond, so this is a floor to lower only with a new measurement, not
    // a number to trim because it looks round.
    //
    // What *was* wrong was the recovery, not the floor; see
    // kQueueDelayDecreasePerSourceFrame below.
    static constexpr int kDefaultBufferMilliseconds = 8;
    static constexpr int kMinimumBufferMilliseconds = 0;
    static constexpr int kMaximumBufferMilliseconds = 32;

    explicit FrameSelector(
        int maximum_multiplier = 6,
        std::chrono::milliseconds buffer_floor =
            std::chrono::milliseconds{kDefaultBufferMilliseconds},
        CeilingPacing ceiling_pacing = CeilingPacing::spread);

    void SetMaximumMultiplier(int maximum_multiplier);
    void SetBufferFloor(std::chrono::milliseconds buffer_floor);
    void SetCeilingPacing(CeilingPacing ceiling_pacing) noexcept;

    // Turns frame generation off without tearing anything down. Selection falls
    // back to the newest real frame, and the slot gate is driven at the source
    // rate instead of the target, so the display sees native frames and nothing
    // else. Capture, the output window, the overlay, and the output clock all
    // stay live, which is the point: this exists so a user can A/B generation on
    // one scene without relaunching and losing it.
    //
    // Expressed as a multiplier of one rather than as a separate path through
    // Select, so the ceiling machinery that already enforces --max-multiplier
    // enforces this too, and nothing new decides which slots reach the display.
    //
    // Deliberately survives Reset: a capture discontinuity is not a reason to
    // start generating again behind the user's back.
    void SetGenerationEnabled(bool enabled) noexcept;
    [[nodiscard]] bool GenerationEnabled() const noexcept;

    // Extra media delay on top of the queue target, for a caller that selects
    // a slot *before* its deadline (PacingMode::queued renders slot k+1 while
    // slot k is being presented). Selecting one slot early means the source
    // frames bracketing that slot's media time must already have arrived one
    // slot earlier than the queue alone guarantees, so the media clock has to
    // sit that much further behind live. It is added to every queue-delay
    // update rather than to the deadline, so the reported queue delay and the
    // HUD's capture-to-present both show the true figure. Zero by default,
    // which is the historical behaviour.
    void SetLookahead(Clock::duration lookahead) noexcept;
    [[nodiscard]] Clock::duration Lookahead() const noexcept;

    void Reset() noexcept;
    [[nodiscard]] int MaximumMultiplier() const noexcept;
    [[nodiscard]] std::chrono::milliseconds BufferFloor() const noexcept;
    [[nodiscard]] CeilingPacing GetCeilingPacing() const noexcept;
    [[nodiscard]] FrameSelection Select(
        const OutputClock::Deadline& deadline,
        FrameRate target_rate,
        const SourceTimeline& timeline) noexcept;

    // Selection for a loop with no output clock (PacingMode::unpaced): the
    // caller presents whenever the swap chain has a free back buffer, and asks
    // what to show *now*. Identical to Select except that the multiplier
    // ceiling's slot gate is not consulted -- there are no slots to ration --
    // so a ceiling hold can never be returned. Generation off still selects a
    // real frame only. The returned deadline carries `now` as its time and
    // slot zero, and no queue-delay bookkeeping differs from Select.
    [[nodiscard]] FrameSelection SelectNow(
        Clock::time_point now,
        FrameRate target_rate,
        const SourceTimeline& timeline) noexcept;

private:
    // Queue target 1 needs one future source endpoint plus enough capture
    // headroom that callback jitter cannot retime the media clock. The floor is
    // user-configurable; underruns grow the adaptive budget and clean source
    // frames decay it. Measured callback delay remains telemetry, not media time.
    static constexpr auto kQueueJitterBudget = std::chrono::milliseconds(8);
    static constexpr auto kMaximumAdaptiveJitterBudget = std::chrono::milliseconds(32);
    static constexpr auto kQueueDelayIncreasePerUnderrun = std::chrono::milliseconds(2);
    // Recovery has to be quick enough that a burst of underruns does not leave
    // latency elevated long after the cause is gone. At 100 us per source frame
    // the budget shed 6 ms/s at 60 FPS, so a walk to the 32 ms ceiling took
    // roughly five seconds to undo. 1 ms per clean source frame undoes the same
    // walk in half a second while still being far slower to fall than to rise,
    // which is the asymmetry that keeps it from oscillating.
    static constexpr auto kQueueDelayDecreasePerSourceFrame = std::chrono::microseconds(1000);
    // At or below unity the source already supplies a distinct frame for every
    // output slot, so synthesising one adds blend artifacts and latency without
    // adding smoothness. The tolerance absorbs measured-rate jitter around unity
    // rather than flip-flopping between passthrough and synthesis.
    //
    // Sized from both ends. A matched source/target measured 0.92..1.06 when
    // capture was healthy and up to ~1.11 under mild load, so anything below
    // about 1.12 leaves real unity cases synthesising. The upper bound is the
    // smallest ratio worth interpolating: a 120 FPS source on a 144 Hz display
    // needs 1.20, and that is a case frame generation exists to serve. 1.15 sits
    // between the two, so unity passes through and 120->144 still interpolates.
    static constexpr double kUnityMultiplierTolerance = 0.15;

    [[nodiscard]] FrameSelection SelectImpl(
        const OutputClock::Deadline& deadline,
        FrameRate target_rate,
        const SourceTimeline& timeline,
        bool enforce_ceiling) noexcept;
    [[nodiscard]] FrameSelection SelectReal(
        FrameSelection selection,
        const SourceFrameSample& frame,
        FrameSelectionMode mode) const noexcept;
    void UpdateQueueDelay(const SourceTimeline& timeline, std::uint64_t newest_sequence) noexcept;
    void IncreaseQueueDelay(const SourceTimeline& timeline) noexcept;

    int maximum_multiplier_ = 6;
    Clock::duration buffer_floor_ = std::chrono::duration_cast<Clock::duration>(
        kQueueJitterBudget);
    Clock::duration adaptive_jitter_budget_ = std::chrono::duration_cast<Clock::duration>(
        kQueueJitterBudget);
    Clock::duration lookahead_ = Clock::duration::zero();
    CeilingPacing ceiling_pacing_ = CeilingPacing::spread;
    bool generation_enabled_ = true;
    std::optional<Clock::duration> queue_delay_;
    std::uint64_t last_queue_delay_update_sequence_ = 0;
    TargetSlotGate slot_gate_;
    bool recovering_from_stall_ = false;
    std::uint64_t recovery_started_at_sequence_ = 0;
};

} // namespace osss
