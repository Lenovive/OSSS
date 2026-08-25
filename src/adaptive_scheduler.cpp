#include "adaptive_scheduler.h"

#include "frame_rate_limits.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace osss {
namespace {

using Clock = OutputClock::Clock;

Clock::duration DurationForRateSlot(const FrameRate rate, const std::uint64_t slot) noexcept {
    if (slot == 0 || !rate.IsValid()) {
        return Clock::duration::zero();
    }

    const long double seconds =
        (static_cast<long double>(slot) * static_cast<long double>(rate.denominator)) /
        static_cast<long double>(rate.numerator);
    const long double clock_ticks =
        seconds * static_cast<long double>(Clock::period::den) /
        static_cast<long double>(Clock::period::num);
    const long double maximum = static_cast<long double>(
        std::numeric_limits<Clock::duration::rep>::max());
    const auto rounded = static_cast<Clock::duration::rep>(
        std::clamp(std::round(clock_ticks), 0.0L, maximum));
    return Clock::duration{rounded};
}

double NonNegativeSeconds(const Clock::duration duration) noexcept {
    return std::max(0.0, std::chrono::duration<double>(duration).count());
}

double PercentileSeconds(
    const std::deque<double>& samples,
    const double percentile) noexcept {
    if (samples.empty()) {
        return 0.0;
    }

    std::vector<double> ordered(samples.begin(), samples.end());
    std::sort(ordered.begin(), ordered.end());
    const double clamped_percentile = std::clamp(percentile, 0.0, 1.0);
    const auto rank = static_cast<std::size_t>(std::ceil(
        clamped_percentile * static_cast<double>(ordered.size())));
    const std::size_t index = rank == 0 ? 0 : std::min(rank - 1, ordered.size() - 1);
    return ordered[index];
}

} // namespace

FrameRate FrameRate::FromFps(const double frames_per_second) {
    if (!IsValidTargetFps(frames_per_second)) {
        throw std::out_of_range("Output target must be between 24 and 1000 FPS.");
    }

    constexpr std::uint64_t scale = 1'000'000;
    const auto numerator = static_cast<std::uint64_t>(
        std::llround(frames_per_second * static_cast<double>(scale)));
    const std::uint64_t divisor = std::gcd(numerator, scale);
    return FrameRate{numerator / divisor, scale / divisor};
}

bool FrameRate::IsValid() const noexcept {
    if (numerator == 0 || denominator == 0) {
        return false;
    }
    return IsValidTargetFps(AsDouble());
}

double FrameRate::AsDouble() const noexcept {
    if (denominator == 0) {
        return 0.0;
    }
    return static_cast<double>(numerator) / static_cast<double>(denominator);
}

OutputClock::OutputClock(const FrameRate rate) {
    SetRate(rate);
}

void OutputClock::SetRate(const FrameRate rate) {
    if (!rate.IsValid()) {
        throw std::out_of_range("Output frame rate must be between 24 and 1000 FPS.");
    }
    rate_ = rate;
}

void OutputClock::Start(const Clock::time_point epoch) noexcept {
    epoch_ = epoch;
    next_slot_ = 0;
    missed_deadlines_ = 0;
    phase_corrections_ = 0;
    last_phase_correction_ = Clock::duration::zero();
    started_ = true;
}

void OutputClock::Reset() noexcept {
    epoch_ = {};
    next_slot_ = 0;
    missed_deadlines_ = 0;
    phase_corrections_ = 0;
    last_phase_correction_ = Clock::duration::zero();
    started_ = false;
}

bool OutputClock::Started() const noexcept {
    return started_;
}

FrameRate OutputClock::Rate() const noexcept {
    return rate_;
}

OutputClock::Clock::time_point OutputClock::NextDeadline() const noexcept {
    return DeadlineForSlot(next_slot_);
}

std::uint64_t OutputClock::NextSlot() const noexcept {
    return next_slot_;
}

OutputClock::Clock::time_point OutputClock::DeadlineForSlot(const std::uint64_t slot) const noexcept {
    return epoch_ + DurationForRateSlot(rate_, slot);
}

std::optional<OutputClock::Deadline> OutputClock::TakeLatestDue(
    const Clock::time_point now) noexcept {
    if (!started_ || now < NextDeadline()) {
        return std::nullopt;
    }

    const long double elapsed_seconds = std::max(
        0.0L,
        std::chrono::duration<long double>(now - epoch_).count());
    const long double elapsed_slots =
        elapsed_seconds * static_cast<long double>(rate_.numerator) /
        static_cast<long double>(rate_.denominator);
    std::uint64_t latest_slot = static_cast<std::uint64_t>(std::floor(elapsed_slots + 1.0e-12L));
    latest_slot = std::max(latest_slot, next_slot_);

    const std::uint64_t skipped = latest_slot - next_slot_;
    missed_deadlines_ += skipped;
    next_slot_ = latest_slot + 1;
    return Deadline{DeadlineForSlot(latest_slot), latest_slot, skipped};
}

bool OutputClock::PhaseAlignToVblank(
    const Clock::time_point vblank,
    const Clock::duration refresh_period,
    const Clock::duration lead) noexcept {
    if (!started_ || refresh_period <= Clock::duration::zero() || !rate_.IsValid()) {
        return false;
    }

    const double slot_seconds =
        static_cast<double>(rate_.denominator) / static_cast<double>(rate_.numerator);
    const double refresh_seconds = std::chrono::duration<double>(refresh_period).count();
    if (!(slot_seconds > 0.0) || !(refresh_seconds > 0.0)) {
        return false;
    }

    // Alignable when one grid's period is a whole multiple of the other's.
    // Either direction counts: 60 FPS on a 120 Hz panel puts a deadline on every
    // second vblank, and 120 FPS on a 60 Hz panel puts one on every vblank plus
    // one between them. 120 against 144 satisfies neither and is rejected.
    const double slots_per_refresh = refresh_seconds / slot_seconds;
    const double refreshes_per_slot = slot_seconds / refresh_seconds;
    const auto commensurate = [](const double ratio) {
        if (!(ratio > 0.0)) {
            return false;
        }
        const double nearest = std::round(ratio);
        return nearest >= 1.0 && std::abs(ratio - nearest) <= kCommensurateTolerance * nearest;
    };
    if (!commensurate(slots_per_refresh) && !commensurate(refreshes_per_slot)) {
        return false;
    }

    // The error is folded modulo the *refresh* period, not the slot period, and
    // that distinction is the whole correctness of this function.
    //
    // The goal is that a deadline plus its lead lands on a vblank. Folding
    // modulo the slot period asks instead that every vblank sit on the deadline
    // grid, which is false whenever there is more than one vblank per slot: at
    // 60 FPS on a 120 Hz panel only every second vblank can carry a deadline,
    // and the ones in between read as half a slot of error. A servo fed that
    // alternating signal chases both and settles on neither.
    //
    // Modulo the refresh period the question becomes "is the grid on the vblank
    // raster", which is well posed in both directions. Where the slot period is
    // a whole multiple of the refresh period every deadline then lands on a
    // vblank; where it is a whole divisor, as many as can do so. The
    // commensurate test above is what guarantees the offset does not drift from
    // one slot to the next, which is the case this cannot help.
    const double offset_seconds =
        std::chrono::duration<double>((epoch_ + lead) - vblank).count();
    double phase = std::fmod(offset_seconds, refresh_seconds);
    if (phase < 0.0) {
        phase += refresh_seconds;
    }
    if (phase > refresh_seconds * 0.5) {
        phase -= refresh_seconds;
    }

    // Negated: `phase` is how far the grid sits *ahead* of where it should be,
    // so the epoch moves back by a fraction of it.
    const auto error = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(-phase * kPhaseCorrectionGain));
    const auto cap = std::chrono::duration_cast<Clock::duration>(kMaximumPhaseCorrection);
    const auto correction = std::clamp(error, -cap, cap);
    if (correction == Clock::duration::zero()) {
        return false;
    }

    epoch_ += correction;
    last_phase_correction_ = correction;
    ++phase_corrections_;
    return true;
}

std::uint64_t OutputClock::PhaseCorrectionCount() const noexcept {
    return phase_corrections_;
}

OutputClock::Clock::duration OutputClock::LastPhaseCorrection() const noexcept {
    return last_phase_correction_;
}

std::uint64_t OutputClock::MissedDeadlineCount() const noexcept {
    return missed_deadlines_;
}

void OutputClock::RecordMissedSubmission(const std::uint64_t count) noexcept {
    missed_deadlines_ += count;
}

SourceTimeline::SourceTimeline(const std::size_t maximum_history)
    : maximum_history_(std::max<std::size_t>(2, maximum_history)) {}

std::optional<std::uint64_t> SourceTimeline::Ingest(
    const std::uint64_t raw_sequence,
    const Clock::time_point media_time,
    const Clock::time_point arrival_time,
    const std::uint32_t width,
    const std::uint32_t height,
    const bool duplicate,
    const Clock::time_point ingest_time) {
    ++raw_frame_count_;
    if (duplicate) {
        ++duplicate_frame_count_;
        return std::nullopt;
    }

    if (!frames_.empty() &&
        (frames_.back().width != width || frames_.back().height != height)) {
        frames_.clear();
        has_period_sample_ = false;
        ++discontinuity_count_;
    }
    if (!frames_.empty() && media_time <= frames_.back().media_time) {
        frames_.clear();
        has_period_sample_ = false;
        ++discontinuity_count_;
    }

    if (!frames_.empty()) {
        const double sample_seconds =
            std::chrono::duration<double>(media_time - frames_.back().media_time).count();
        if (sample_seconds >= kMinimumPeriodSeconds && sample_seconds <= kMaximumPeriodSeconds) {
            source_period_seconds_ = has_period_sample_
                ? ((1.0 - kPeriodSampleWeight) * source_period_seconds_) +
                    (kPeriodSampleWeight * sample_seconds)
                : sample_seconds;
            has_period_sample_ = true;
        }
    }

    const double capture_delay = NonNegativeSeconds(arrival_time - media_time);
    capture_delay_seconds_ = has_capture_delay_sample_
        ? ((1.0 - kCaptureDelaySampleWeight) * capture_delay_seconds_) +
            (kCaptureDelaySampleWeight * capture_delay)
        : capture_delay;
    has_capture_delay_sample_ = true;

    if (ingest_time != Clock::time_point{} && ingest_time >= media_time) {
        media_to_ingest_seconds_.push_back(
            std::chrono::duration<double>(ingest_time - media_time).count());
        while (media_to_ingest_seconds_.size() > kLatencySampleWindow) {
            media_to_ingest_seconds_.pop_front();
        }
    }

    const std::uint64_t unique_sequence = ++unique_frame_count_;
    frames_.push_back(SourceFrameSample{
        raw_sequence,
        unique_sequence,
        media_time,
        arrival_time,
        ingest_time,
        width,
        height,
    });
    while (frames_.size() > maximum_history_) {
        frames_.pop_front();
    }
    return unique_sequence;
}

void SourceTimeline::ResetHistory() noexcept {
    frames_.clear();
    has_period_sample_ = false;
    source_period_seconds_ = kDefaultPeriodSeconds;
    media_to_ingest_seconds_.clear();
    ++discontinuity_count_;
}

const std::deque<SourceFrameSample>& SourceTimeline::Frames() const noexcept {
    return frames_;
}

std::uint64_t SourceTimeline::RawFrameCount() const noexcept {
    return raw_frame_count_;
}

std::uint64_t SourceTimeline::UniqueFrameCount() const noexcept {
    return unique_frame_count_;
}

std::uint64_t SourceTimeline::DuplicateFrameCount() const noexcept {
    return duplicate_frame_count_;
}

std::uint64_t SourceTimeline::DiscontinuityCount() const noexcept {
    return discontinuity_count_;
}

double SourceTimeline::EstimatedSourceFps() const noexcept {
    return 1.0 / std::max(source_period_seconds_, std::numeric_limits<double>::epsilon());
}

std::chrono::duration<double> SourceTimeline::EstimatedSourcePeriod() const noexcept {
    return std::chrono::duration<double>(source_period_seconds_);
}

std::chrono::duration<double> SourceTimeline::CaptureDelay() const noexcept {
    return std::chrono::duration<double>(capture_delay_seconds_);
}

std::chrono::duration<double> SourceTimeline::MediaToIngestP50() const noexcept {
    return std::chrono::duration<double>(PercentileSeconds(media_to_ingest_seconds_, 0.50));
}

std::chrono::duration<double> SourceTimeline::MediaToIngestP95() const noexcept {
    return std::chrono::duration<double>(PercentileSeconds(media_to_ingest_seconds_, 0.95));
}

std::size_t SourceTimeline::MediaToIngestSampleCount() const noexcept {
    return media_to_ingest_seconds_.size();
}

bool SourceTimeline::IsStalled(const Clock::time_point now) const noexcept {
    if (frames_.empty()) {
        return false;
    }
    if (has_period_sample_ && source_period_seconds_ >= kStallPeriodSeconds) {
        return true;
    }
    const double silence_seconds = NonNegativeSeconds(now - frames_.back().arrival_time);
    const double silence_limit = std::max(kStallPeriodSeconds, source_period_seconds_ * 3.0);
    return silence_seconds >= silence_limit;
}

PacingMonitor::PacingMonitor(const std::size_t window)
    : window_(std::max<std::size_t>(2, window)) {}

void PacingMonitor::RecordPresent(const Clock::time_point when) noexcept {
    if (!has_last_present_) {
        last_present_ = when;
        has_last_present_ = true;
        return;
    }
    // A non-monotonic timestamp is a caller error rather than a pacing event.
    // Re-baseline instead of recording a negative interval, which would drag
    // every percentile below the target and read as suspiciously good pacing.
    if (when < last_present_) {
        last_present_ = when;
        return;
    }

    intervals_seconds_.push_back(std::chrono::duration<double>(when - last_present_).count());
    last_present_ = when;
    while (intervals_seconds_.size() > window_) {
        intervals_seconds_.pop_front();
    }
}

void PacingMonitor::Reset() noexcept {
    intervals_seconds_.clear();
    has_last_present_ = false;
    last_present_ = {};
}

PacingMonitor::Report PacingMonitor::Summarize(const double target_fps) const noexcept {
    Report report{};
    report.sample_count = intervals_seconds_.size();
    if (intervals_seconds_.empty()) {
        return report;
    }

    report.p50_milliseconds = PercentileSeconds(intervals_seconds_, 0.50) * 1000.0;
    report.p95_milliseconds = PercentileSeconds(intervals_seconds_, 0.95) * 1000.0;
    report.maximum_milliseconds =
        *std::max_element(intervals_seconds_.begin(), intervals_seconds_.end()) * 1000.0;

    if (!(target_fps > 0.0)) {
        return report;
    }
    const double target_period = 1.0 / target_fps;
    const double tolerance = target_period * kOnTimeTolerance;

    std::size_t on_time = 0;
    double absolute_error = 0.0;
    for (const double interval : intervals_seconds_) {
        const double error = std::abs(interval - target_period);
        absolute_error += error;
        if (error <= tolerance) {
            ++on_time;
        }
    }
    report.on_time_fraction =
        static_cast<double>(on_time) / static_cast<double>(intervals_seconds_.size());
    report.mean_absolute_error_milliseconds =
        absolute_error / static_cast<double>(intervals_seconds_.size()) * 1000.0;
    return report;
}

std::size_t PacingMonitor::SampleCount() const noexcept {
    return intervals_seconds_.size();
}

void TargetSlotGate::Reset() noexcept {
    credit_ = 0.0;
    initialized_ = false;
    even_slot_ = 0;
    even_interval_ = 0;
    even_initialized_ = false;
}

bool TargetSlotGate::Allow(const double allowed_rate, const double target_rate) noexcept {
    if (!(target_rate > 0.0) || allowed_rate >= target_rate - 1.0e-9) {
        credit_ = 0.0;
        initialized_ = false;
        return true;
    }
    if (!(allowed_rate > 0.0)) {
        return false;
    }

    const double ratio = std::clamp(allowed_rate / target_rate, 0.0, 1.0);
    if (!initialized_) {
        credit_ = 1.0 - ratio;
        initialized_ = true;
    }
    credit_ += ratio;
    if (credit_ + 1.0e-12 < 1.0) {
        return false;
    }
    credit_ = std::min(1.0, credit_ - 1.0);
    return true;
}

bool TargetSlotGate::AllowEven(
    const double allowed_rate,
    const double target_rate) noexcept {
    if (!(target_rate > 0.0) || allowed_rate >= target_rate - 1.0e-9) {
        even_slot_ = 0;
        even_interval_ = 0;
        even_initialized_ = false;
        return true;
    }
    if (!(allowed_rate > 0.0)) {
        return false;
    }

    const auto interval = static_cast<std::uint64_t>(std::max(
        1.0,
        std::ceil(target_rate / allowed_rate - 1.0e-12)));
    if (!even_initialized_ || even_interval_ != interval) {
        even_slot_ = 0;
        even_interval_ = interval;
        even_initialized_ = true;
    }

    const bool allowed = even_slot_ == 0;
    even_slot_ = (even_slot_ + 1) % even_interval_;
    return allowed;
}

FrameSelector::FrameSelector(
    const int maximum_multiplier,
    const std::chrono::milliseconds buffer_floor,
    const CeilingPacing ceiling_pacing) {
    SetMaximumMultiplier(maximum_multiplier);
    SetBufferFloor(buffer_floor);
    SetCeilingPacing(ceiling_pacing);
}

void FrameSelector::SetGenerationEnabled(const bool enabled) noexcept {
    if (enabled == generation_enabled_) {
        return;
    }
    generation_enabled_ = enabled;
    // The gate is holding credit for a rate that is about to change by the whole
    // multiplier. Carrying it across would spend the first slots after the
    // switch on the old rate's schedule -- a visible burst turning generation
    // back on, and a stall turning it off.
    slot_gate_.Reset();
}

bool FrameSelector::GenerationEnabled() const noexcept {
    return generation_enabled_;
}

void FrameSelector::SetMaximumMultiplier(const int maximum_multiplier) {
    if (!IsValidMultiplier(maximum_multiplier)) {
        throw std::out_of_range("Maximum frame multiplier must be between 2 and 20.");
    }
    maximum_multiplier_ = maximum_multiplier;
    slot_gate_.Reset();
}

void FrameSelector::SetBufferFloor(const std::chrono::milliseconds buffer_floor) {
    if (buffer_floor.count() < kMinimumBufferMilliseconds ||
        buffer_floor.count() > kMaximumBufferMilliseconds) {
        throw std::out_of_range(
            "Buffer floor must be between 0 and 32 milliseconds.");
    }
    buffer_floor_ = std::chrono::duration_cast<Clock::duration>(buffer_floor);
    adaptive_jitter_budget_ = buffer_floor_;
    queue_delay_.reset();
    last_queue_delay_update_sequence_ = 0;
}

void FrameSelector::SetCeilingPacing(const CeilingPacing ceiling_pacing) noexcept {
    ceiling_pacing_ = ceiling_pacing;
    slot_gate_.Reset();
}

void FrameSelector::Reset() noexcept {
    queue_delay_.reset();
    adaptive_jitter_budget_ = buffer_floor_;
    last_queue_delay_update_sequence_ = 0;
    slot_gate_.Reset();
    recovering_from_stall_ = false;
    recovery_started_at_sequence_ = 0;
}

int FrameSelector::MaximumMultiplier() const noexcept {
    return maximum_multiplier_;
}

std::chrono::milliseconds FrameSelector::BufferFloor() const noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(buffer_floor_);
}

FrameSelector::CeilingPacing FrameSelector::GetCeilingPacing() const noexcept {
    return ceiling_pacing_;
}

void FrameSelector::UpdateQueueDelay(
    const SourceTimeline& timeline,
    const std::uint64_t newest_sequence) noexcept {
    if (newest_sequence == last_queue_delay_update_sequence_) {
        return;
    }

    const Clock::duration source_period = std::chrono::duration_cast<Clock::duration>(
        timeline.EstimatedSourcePeriod());
    if (queue_delay_ && adaptive_jitter_budget_ > buffer_floor_) {
        adaptive_jitter_budget_ = std::max(
            buffer_floor_,
            adaptive_jitter_budget_ - std::chrono::duration_cast<Clock::duration>(
                kQueueDelayDecreasePerSourceFrame));
    }
    queue_delay_ =
        std::max(source_period, Clock::duration{1}) + adaptive_jitter_budget_ + lookahead_;
    last_queue_delay_update_sequence_ = newest_sequence;
}

void FrameSelector::IncreaseQueueDelay(const SourceTimeline& timeline) noexcept {
    adaptive_jitter_budget_ = std::min(
        std::chrono::duration_cast<Clock::duration>(kMaximumAdaptiveJitterBudget),
        adaptive_jitter_budget_ + std::chrono::duration_cast<Clock::duration>(
            kQueueDelayIncreasePerUnderrun));
    const Clock::duration source_period = std::chrono::duration_cast<Clock::duration>(
        timeline.EstimatedSourcePeriod());
    queue_delay_ =
        std::max(source_period, Clock::duration{1}) + adaptive_jitter_budget_ + lookahead_;
}

void FrameSelector::SetLookahead(const Clock::duration lookahead) noexcept {
    lookahead_ = std::max(lookahead, Clock::duration::zero());
    // The next UpdateQueueDelay folds it in; forcing a refresh here would need
    // a timeline, and the first Select after a change re-derives it anyway.
    queue_delay_.reset();
    last_queue_delay_update_sequence_ = 0;
}

FrameSelector::Clock::duration FrameSelector::Lookahead() const noexcept {
    return lookahead_;
}

FrameSelection FrameSelector::Select(
    const OutputClock::Deadline& deadline,
    const FrameRate target_rate,
    const SourceTimeline& timeline) noexcept {
    return SelectImpl(deadline, target_rate, timeline, true);
}

FrameSelection FrameSelector::SelectNow(
    const Clock::time_point now,
    const FrameRate target_rate,
    const SourceTimeline& timeline) noexcept {
    OutputClock::Deadline deadline{};
    deadline.time = now;
    return SelectImpl(deadline, target_rate, timeline, false);
}

FrameSelection FrameSelector::SelectImpl(
    const OutputClock::Deadline& deadline,
    const FrameRate target_rate,
    const SourceTimeline& timeline,
    const bool enforce_ceiling) noexcept {
    FrameSelection selection{};
    selection.deadline = deadline;
    selection.queue_occupancy = timeline.Frames().size();

    const auto& frames = timeline.Frames();
    if (frames.empty()) {
        return selection;
    }

    const double source_fps = timeline.EstimatedSourceFps();
    const double target_fps = target_rate.AsDouble();
    // Generation off is exactly a multiplier of one: every output slot the
    // source cannot fill from a real frame is declined rather than synthesised.
    const double allowed_rate = generation_enabled_
        ? std::min(target_fps, source_fps * static_cast<double>(maximum_multiplier_))
        : std::min(target_fps, source_fps);
    selection.required_multiplier = target_fps / std::max(source_fps, 1.0e-9);
    selection.allowed_multiplier = allowed_rate / std::max(source_fps, 1.0e-9);

    const SourceFrameSample& newest = frames.back();
    if (timeline.IsStalled(deadline.time)) {
        recovering_from_stall_ = true;
        recovery_started_at_sequence_ = newest.unique_sequence;
        queue_delay_.reset();
        adaptive_jitter_budget_ = buffer_floor_;
        last_queue_delay_update_sequence_ = 0;
        return SelectReal(std::move(selection), newest, FrameSelectionMode::stalled);
    }

    if (recovering_from_stall_) {
        if (newest.unique_sequence < recovery_started_at_sequence_ + 2 || frames.size() < 2) {
            return SelectReal(std::move(selection), newest, FrameSelectionMode::hold);
        }
        recovering_from_stall_ = false;
        queue_delay_.reset();
        adaptive_jitter_budget_ = buffer_floor_;
        last_queue_delay_update_sequence_ = 0;
    }

    if (frames.size() >= 2) {
        UpdateQueueDelay(timeline, newest.unique_sequence);
    }
    if (queue_delay_) {
        selection.queue_delay = std::chrono::duration<double>(*queue_delay_);
    }

    // Without an output clock there are no slots to ration, so the gate is not
    // even advanced: SelectNow must leave it exactly where a later Select would
    // expect to find it.
    const bool slot_allowed = !enforce_ceiling ||
        (ceiling_pacing_ == CeilingPacing::even
            ? slot_gate_.AllowEven(allowed_rate, target_fps)
            : slot_gate_.Allow(allowed_rate, target_fps));
    if (!slot_allowed) {
        selection.mode = FrameSelectionMode::ceiling_hold;
        selection.previous_unique_sequence = newest.unique_sequence;
        selection.current_unique_sequence = newest.unique_sequence;
        selection.media_time = newest.media_time;
        selection.alpha = 1.0F;
        selection.submit = false;
        return selection;
    }

    // Placed after the gate so the declined slots are still counted as ceiling
    // holds, and after UpdateQueueDelay so the queue stays warm the whole time
    // generation is off -- switching back on then resumes on an established
    // delay instead of rebuilding one from cold.
    if (!generation_enabled_) {
        return SelectReal(std::move(selection), newest, FrameSelectionMode::real_frame);
    }

    if (frames.size() == 1) {
        return SelectReal(std::move(selection), newest, FrameSelectionMode::cold_start);
    }

    selection.media_time = deadline.time - *queue_delay_;

    if (selection.media_time <= frames.front().media_time) {
        return SelectReal(
            std::move(selection),
            frames.front(),
            FrameSelectionMode::cold_start);
    }

    const auto upper = std::upper_bound(
        frames.begin(),
        frames.end(),
        selection.media_time,
        [](const Clock::time_point media_time, const SourceFrameSample& frame) {
            return media_time < frame.media_time;
        });
    if (upper == frames.end()) {
        IncreaseQueueDelay(timeline);
        selection.queue_delay = std::chrono::duration<double>(*queue_delay_);
        return SelectReal(std::move(selection), newest, FrameSelectionMode::underrun);
    }

    const SourceFrameSample& current = *upper;
    const SourceFrameSample& previous = *std::prev(upper);
    const double pair_seconds =
        std::chrono::duration<double>(current.media_time - previous.media_time).count();
    if (!(pair_seconds > 0.0)) {
        return SelectReal(std::move(selection), current, FrameSelectionMode::real_frame);
    }

    const double elapsed_seconds =
        std::chrono::duration<double>(selection.media_time - previous.media_time).count();
    const double alpha = std::clamp(elapsed_seconds / pair_seconds, 0.0, 1.0);
    selection.previous_unique_sequence = previous.unique_sequence;
    selection.current_unique_sequence = current.unique_sequence;
    selection.alpha = static_cast<float>(alpha);
    selection.submit = true;

    // Unity passthrough: when the target rate does not exceed the measured
    // source rate there is a real frame available for every output slot, so
    // interpolating between two of them can only soften edges and add latency.
    // Present the nearer real endpoint instead of a synthesised blend.
    if (selection.required_multiplier <= 1.0 + kUnityMultiplierTolerance) {
        return SelectReal(
            std::move(selection),
            alpha >= 0.5 ? current : previous,
            FrameSelectionMode::real_frame);
    }

    if (alpha <= 1.0e-6) {
        selection.mode = FrameSelectionMode::real_frame;
        selection.current_unique_sequence = previous.unique_sequence;
        selection.alpha = 1.0F;
    } else if (alpha >= 1.0 - 1.0e-6) {
        selection.mode = FrameSelectionMode::real_frame;
        selection.previous_unique_sequence = current.unique_sequence;
        selection.alpha = 1.0F;
    } else {
        selection.mode = FrameSelectionMode::interpolate;
    }
    return selection;
}

FrameSelection FrameSelector::SelectReal(
    FrameSelection selection,
    const SourceFrameSample& frame,
    const FrameSelectionMode mode) const noexcept {
    selection.mode = mode;
    selection.media_time = frame.media_time;
    selection.previous_unique_sequence = frame.unique_sequence;
    selection.current_unique_sequence = frame.unique_sequence;
    selection.alpha = 1.0F;
    selection.submit = true;
    return selection;
}

} // namespace osss
