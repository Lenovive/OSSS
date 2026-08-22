#include "capture_smoke.h"

#include "adaptive_scheduler.h"
#include "capture_session.h"
#include "renderer.h"
#include "window_catalog.h"

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <thread>

namespace osss {
namespace {

constexpr wchar_t kSmokeWindowClass[] = L"OSSS.CaptureSmokeSource";

struct SmokeState {
    int animation_frame = 0;
};

LRESULT CALLBACK SmokeWindowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
    auto* state = reinterpret_cast<SmokeState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<SmokeState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);

        const HBRUSH background = CreateSolidBrush(RGB(12, 18, 30));
        FillRect(dc, &client, background);
        DeleteObject(background);

        if (state) {
            const int width = client.right - client.left;
            const int position = (state->animation_frame * 13) % std::max(1, width - 70);
            RECT moving_box{position, 45, position + 70, 115};
            const HBRUSH foreground = CreateSolidBrush(RGB(60, 210, 255));
            FillRect(dc, &moving_box, foreground);
            DeleteObject(foreground);
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

void PumpSmokeMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

class AdaptiveSmokeWaiter {
public:
    AdaptiveSmokeWaiter() {
        timer_ = CreateWaitableTimerExW(
            nullptr,
            nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_ALL_ACCESS);
        if (!timer_ && GetLastError() == ERROR_INVALID_PARAMETER) {
            timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        }
        if (!timer_) {
            winrt::throw_last_error();
        }
    }

    ~AdaptiveSmokeWaiter() {
        if (timer_) {
            CloseHandle(timer_);
        }
    }

    AdaptiveSmokeWaiter(const AdaptiveSmokeWaiter&) = delete;
    AdaptiveSmokeWaiter& operator=(const AdaptiveSmokeWaiter&) = delete;

    void WaitUntil(
        const std::chrono::steady_clock::time_point deadline,
        const HANDLE capture_event) {
        using HundredNanoseconds =
            std::chrono::duration<LONGLONG, std::ratio<1, 10'000'000>>;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return;
        }

        LARGE_INTEGER due_time{};
        due_time.QuadPart = -std::max<LONGLONG>(
            1,
            std::chrono::duration_cast<HundredNanoseconds>(deadline - now).count());
        if (!SetWaitableTimer(timer_, &due_time, 0, nullptr, nullptr, FALSE)) {
            winrt::throw_last_error();
        }

        HANDLE handles[2] = {timer_, capture_event};
        const DWORD handle_count = capture_event ? 2 : 1;
        const DWORD result = MsgWaitForMultipleObjectsEx(
            handle_count,
            handles,
            INFINITE,
            QS_ALLINPUT,
            MWMO_ALERTABLE | MWMO_INPUTAVAILABLE);
        CancelWaitableTimer(timer_);
        if ((result >= WAIT_OBJECT_0 && result <= WAIT_OBJECT_0 + handle_count) ||
            result == WAIT_IO_COMPLETION) {
            return;
        }
        winrt::throw_last_error();
    }

private:
    HANDLE timer_ = nullptr;
};

class AdaptiveSmokeSource {
public:
    AdaptiveSmokeSource() {
        std::promise<HWND> ready;
        auto future = ready.get_future();
        thread_ = std::jthread(
            [this, ready = std::move(ready)](const std::stop_token stop) mutable {
                Run(stop, std::move(ready));
            });
        window_ = future.get();
    }

    ~AdaptiveSmokeSource() {
        Stop();
    }

    AdaptiveSmokeSource(const AdaptiveSmokeSource&) = delete;
    AdaptiveSmokeSource& operator=(const AdaptiveSmokeSource&) = delete;

    [[nodiscard]] HWND Window() const noexcept {
        return window_;
    }

    void Stop() noexcept {
        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }
        window_ = nullptr;
    }

private:
    static void Run(
        const std::stop_token stop,
        std::promise<HWND> ready) noexcept {
        using Clock = std::chrono::steady_clock;
        using namespace std::chrono_literals;

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        bool class_registered = false;
        bool ready_delivered = false;
        HWND window = nullptr;
        try {
            WNDCLASSEXW window_class{};
            window_class.cbSize = sizeof(window_class);
            window_class.lpfnWndProc = SmokeWindowProcedure;
            window_class.hInstance = instance;
            window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            window_class.lpszClassName = kSmokeWindowClass;
            if (RegisterClassExW(&window_class) == 0) {
                if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                    winrt::throw_last_error();
                }
            } else {
                class_registered = true;
            }

            SmokeState state{};
            window = CreateWindowExW(
                WS_EX_TOOLWINDOW,
                kSmokeWindowClass,
                L"OSSS Adaptive Capture Smoke Source",
                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                360,
                220,
                nullptr,
                nullptr,
                instance,
                &state);
            if (!window) {
                winrt::throw_last_error();
            }

            ShowWindow(window, SW_SHOWNORMAL);
            UpdateWindow(window);
            DwmFlush();
            ready.set_value(window);
            ready_delivered = true;

            constexpr auto animation_period = 16666667ns;
            AdaptiveSmokeWaiter waiter;
            Clock::time_point next_animation = Clock::now() + animation_period;
            while (!stop.stop_requested() && IsWindow(window)) {
                PumpSmokeMessages();
                const Clock::time_point now = Clock::now();
                if (now >= next_animation) {
                    ++state.animation_frame;
                    RedrawWindow(
                        window,
                        nullptr,
                        nullptr,
                        RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
                    DwmFlush();
                    do {
                        next_animation += animation_period;
                    } while (next_animation <= now);
                }
                waiter.WaitUntil(next_animation, nullptr);
            }
        } catch (...) {
            if (!ready_delivered) {
                try {
                    ready.set_exception(std::current_exception());
                } catch (...) {
                }
            }
        }

        if (window && IsWindow(window)) {
            DestroyWindow(window);
        }
        PumpSmokeMessages();
        if (class_registered) {
            UnregisterClassW(kSmokeWindowClass, instance);
        }
    }

    std::jthread thread_;
    HWND window_ = nullptr;
};

} // namespace

int RunCaptureSmokeTest() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = SmokeWindowProcedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = kSmokeWindowClass;
    if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        winrt::throw_last_error();
    }

    SmokeState state{};
    const HWND source_window = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kSmokeWindowClass,
        L"OSSS Capture Smoke Source",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        360,
        220,
        nullptr,
        nullptr,
        instance,
        &state);
    if (!source_window) {
        winrt::throw_last_error();
    }

    ShowWindow(source_window, SW_SHOWNORMAL);
    UpdateWindow(source_window);
    DwmFlush();

    bool passed = false;
    bool requested_target_cadence = false;
    try {
        Renderer renderer;
        renderer.InitializeDevice();
        if (!renderer.InterpolatorError().empty()) {
            throw std::runtime_error(
                "Motion interpolator initialization failed: " + renderer.InterpolatorError());
        }
        RECT source_bounds{};
        if (!GetWindowRect(source_window, &source_bounds)) {
            winrt::throw_last_error();
        }
        renderer.CreateOutputWindow(source_bounds, 6, 240.0);
        CaptureSession capture(renderer.Device());
        capture.Start(source_window, 240.0);
        requested_target_cadence = capture.RequestedUpdateFps().has_value();

        std::uint64_t consumed_sequence = 0;
        std::uint64_t presented_frames = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline && IsWindow(source_window)) {
            ++state.animation_frame;
            InvalidateRect(source_window, nullptr, FALSE);
            UpdateWindow(source_window);
            DwmFlush();
            PumpSmokeMessages();

            CapturedFrameInfo information{};
            if (capture.ConsumeLatestAfter(
                    consumed_sequence,
                    information,
                    [&renderer](ID3D11Texture2D* texture) {
                        renderer.PushCapturedFrame(texture);
                    })) {
                consumed_sequence = information.sequence;
                renderer.Show();
            }

            if (renderer.HasCapturedFrame()) {
                renderer.Render(0.5F);
                renderer.Present();
                ++presented_frames;
            }
            if (!renderer.InterpolatorError().empty()) {
                throw std::runtime_error(
                    "Captured-frame motion estimation failed: " + renderer.InterpolatorError());
            }
            if (consumed_sequence >= 4 && presented_frames >= 2) {
                passed = true;
                break;
            }

            const std::string error = capture.Error();
            if (!error.empty()) {
                throw std::runtime_error("Capture callback failed: " + error);
            }
            Sleep(16);
        }
    } catch (...) {
        DestroyWindow(source_window);
        UnregisterClassW(kSmokeWindowClass, instance);
        throw;
    }

    DestroyWindow(source_window);
    UnregisterClassW(kSmokeWindowClass, instance);
    if (!passed) {
        throw std::runtime_error("Windows Graphics Capture did not deliver four frames within five seconds.");
    }

    std::cout
        << "Windows Graphics Capture, motion interpolation, and swap-chain presentation smoke test passed ("
        << (requested_target_cadence
            ? "240 FPS capture interval requested"
            : "system-default capture interval")
        << ").\n";
    return 0;
}

int RunAdaptiveCaptureSmokeTest() {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    AdaptiveSmokeSource source;
    const HWND source_window = source.Window();

    try {
        constexpr FrameRate target_rate{144, 1};
        constexpr int maximum_multiplier = 6;
        constexpr auto warmup = 2s;
        constexpr auto measurement = 10s;

        Renderer renderer;
        renderer.InitializeDevice();

        RECT source_bounds{};
        if (!GetWindowRect(source_window, &source_bounds)) {
            winrt::throw_last_error();
        }
        renderer.CreateOutputWindow(source_bounds, maximum_multiplier, target_rate.AsDouble());
        CaptureSession capture(renderer.Device());
        capture.Start(source_window, target_rate.AsDouble());
        AdaptiveSmokeWaiter waiter;

        OutputClock output_clock(target_rate);
        SourceTimeline timeline;
        FrameSelector selector(maximum_multiplier);
        const Clock::time_point started = Clock::now();
        const Clock::time_point measurement_started = started + warmup;
        const Clock::time_point finished = measurement_started + measurement;
        output_clock.Start(started);
        std::uint64_t consumed_raw_sequence = 0;
        std::uint64_t observed_discontinuities = 0;
        std::uint64_t measured_submissions_at_start = 0;
        std::uint64_t measured_confirmed_at_start = 0;
        std::uint64_t measured_unique_at_start = 0;
        std::uint64_t measured_raw_at_start = 0;
        std::uint64_t measured_duplicates_at_start = 0;
        std::uint64_t measured_drops_at_start = 0;
        std::uint64_t measured_missed_at_start = 0;
        std::uint64_t measured_flow_preparations_at_start = 0;
        std::uint64_t measured_slot_start = 0;
        std::uint64_t measured_slot_end = 0;
        std::size_t maximum_queue_occupancy = 0;
        std::uint64_t interpolation_decisions = 0;
        std::uint64_t latency_sample_count = 0;
        double latency_sum_milliseconds = 0.0;
        double maximum_latency_milliseconds = 0.0;
        double last_queue_delay_milliseconds = 0.0;
        bool measurement_baseline_recorded = false;
        bool alpha_out_of_range = false;

        while (Clock::now() < finished && IsWindow(source_window)) {
            PumpSmokeMessages();

            capture.DrainClassifiedFramesAfter(
                consumed_raw_sequence,
                [&](ID3D11Texture2D* texture, const CapturedFrameInfo& information) {
                    consumed_raw_sequence = std::max(
                        consumed_raw_sequence,
                        information.sequence);
                    const auto unique_sequence = timeline.Ingest(
                        information.sequence,
                        information.media_time,
                        information.arrival,
                        information.width,
                        information.height,
                        information.duplicate,
                        information.ingest);
                    if (timeline.DiscontinuityCount() != observed_discontinuities) {
                        observed_discontinuities = timeline.DiscontinuityCount();
                        selector.Reset();
                    }
                    if (unique_sequence) {
                        renderer.StoreCapturedFrame(texture, *unique_sequence);
                        renderer.Show();
                    }
                });

            if (const auto deadline = output_clock.TakeLatestDue(Clock::now())) {
                if (!measurement_baseline_recorded && deadline->time >= measurement_started) {
                    const auto& present = renderer.PresentationStatistics();
                    measurement_baseline_recorded = true;
                    measured_submissions_at_start = present.submitted;
                    measured_confirmed_at_start = present.confirmed;
                    measured_unique_at_start = timeline.UniqueFrameCount();
                    measured_raw_at_start = capture.CapturedFrameCount();
                    measured_duplicates_at_start = capture.DuplicateFrameCount();
                    measured_drops_at_start = capture.DroppedFrameCount();
                    measured_missed_at_start = output_clock.MissedDeadlineCount();
                    measured_flow_preparations_at_start = renderer.MotionPreparationCount();
                    measured_slot_start = deadline->slot;
                }
                if (measurement_baseline_recorded) {
                    measured_slot_end = deadline->slot;
                }

                const FrameSelection selection = selector.Select(*deadline, target_rate, timeline);
                last_queue_delay_milliseconds = selection.queue_delay.count() * 1000.0;
                maximum_queue_occupancy = std::max(
                    maximum_queue_occupancy,
                    selection.queue_occupancy);
                if (selection.alpha < 0.0F || selection.alpha > 1.0F) {
                    alpha_out_of_range = true;
                }
                if (selection.submit) {
                    const bool selected = selection.mode == FrameSelectionMode::interpolate
                        ? renderer.SelectFramePair(
                            selection.previous_unique_sequence,
                            selection.current_unique_sequence)
                        : renderer.SelectRealFrame(selection.current_unique_sequence);
                    if (selected && renderer.WaitForPresentationSlot(0)) {
                        renderer.Render(selection.alpha);
                        renderer.Present();
                        if (measurement_baseline_recorded) {
                            const double latency_milliseconds = std::max(
                                0.0,
                                std::chrono::duration<double, std::milli>(
                                    Clock::now() - selection.media_time).count());
                            latency_sum_milliseconds += latency_milliseconds;
                            maximum_latency_milliseconds = std::max(
                                maximum_latency_milliseconds,
                                latency_milliseconds);
                            ++latency_sample_count;
                        }
                        if (selection.mode == FrameSelectionMode::interpolate) {
                            ++interpolation_decisions;
                        }
                    } else {
                        output_clock.RecordMissedSubmission();
                    }
                } else if (selection.mode == FrameSelectionMode::no_frame) {
                    output_clock.RecordMissedSubmission();
                }
            }

            const std::string capture_error = capture.Error();
            if (!capture_error.empty()) {
                throw std::runtime_error("Adaptive capture callback failed: " + capture_error);
            }
            const std::string interpolation_error = renderer.InterpolatorError();
            if (!interpolation_error.empty()) {
                throw std::runtime_error(
                    "Adaptive motion preparation failed: " + interpolation_error);
            }
            waiter.WaitUntil(
                std::min(output_clock.NextDeadline(), finished),
                capture.FrameAvailableEvent());
        }

        const auto& present = renderer.PresentationStatistics();
        const double measured_seconds = std::chrono::duration<double>(measurement).count();
        const std::uint64_t submitted = present.submitted - measured_submissions_at_start;
        const std::uint64_t confirmed = present.confirmed - measured_confirmed_at_start;
        const std::uint64_t unique = timeline.UniqueFrameCount() - measured_unique_at_start;
        const std::uint64_t raw = capture.CapturedFrameCount() - measured_raw_at_start;
        const std::uint64_t duplicates =
            capture.DuplicateFrameCount() - measured_duplicates_at_start;
        const std::uint64_t drops = capture.DroppedFrameCount() - measured_drops_at_start;
        const std::uint64_t missed =
            output_clock.MissedDeadlineCount() - measured_missed_at_start;
        const std::uint64_t flow_preparations =
            renderer.MotionPreparationCount() - measured_flow_preparations_at_start;
        const std::uint64_t scheduled_slots = measured_slot_end >= measured_slot_start
            ? measured_slot_end - measured_slot_start + 1
            : 0;
        const double scheduled_fps = static_cast<double>(scheduled_slots) / measured_seconds;
        const double submitted_fps = static_cast<double>(submitted) / measured_seconds;
        const double unique_fps = static_cast<double>(unique) / measured_seconds;
        const double raw_fps = static_cast<double>(raw) / measured_seconds;
        const double average_latency_milliseconds = latency_sample_count > 0
            ? latency_sum_milliseconds / static_cast<double>(latency_sample_count)
            : 0.0;
        const auto display = WindowDisplayRefreshInfo(source_window);
        const bool display_can_accept_target =
            display && display->active_rate.AsDouble() >= target_rate.AsDouble() - 0.5;

        // Name the clause that failed and show the measurements behind it.
        // A bare "contract failed" forces whoever hits this to re-instrument the
        // run before they can even tell whether it is a source regression or a
        // loaded machine, and this check is documented as hardware dependent --
        // which makes telling those apart the whole job.
        {
            std::string failures;
            const auto note = [&failures](const bool failed, const std::string& detail) {
                if (!failed) {
                    return;
                }
                if (!failures.empty()) {
                    failures += "; ";
                }
                failures += detail;
            };
            note(!measurement_baseline_recorded, "no measurement baseline was recorded");
            note(std::abs(scheduled_fps - 144.0) > 1.0,
                "scheduled " + std::to_string(scheduled_fps) + " fps is not within 1.0 of 144.0");
            note(alpha_out_of_range, "an interpolation alpha left [0, 1]");
            note(maximum_queue_occupancy > 8,
                "queue occupancy peaked at " + std::to_string(maximum_queue_occupancy) +
                    ", above 8");
            note(unique < 30, "only " + std::to_string(unique) + " unique source frames, under 30");
            note(raw < unique,
                "raw " + std::to_string(raw) + " is below unique " + std::to_string(unique));
            note(interpolation_decisions == 0, "no interpolation decision was taken");
            note(flow_preparations > unique + 2,
                "flow ran " + std::to_string(flow_preparations) + " times for " +
                    std::to_string(unique) + " unique frames, which is more than once per pair");
            if (!failures.empty()) {
                throw std::runtime_error(
                    "Adaptive capture timing contract failed: " + failures +
                    ". Measured: scheduled=" + std::to_string(scheduled_fps) +
                    " submitted=" + std::to_string(submitted_fps) +
                    " raw=" + std::to_string(raw_fps) +
                    " unique=" + std::to_string(unique_fps) +
                    " duplicates=" + std::to_string(duplicates) +
                    " capture-drops=" + std::to_string(drops) +
                    " missed=" + std::to_string(missed) +
                    " max-queue=" + std::to_string(maximum_queue_occupancy) +
                    " interpolation-decisions=" + std::to_string(interpolation_decisions) +
                    " flow-preparations=" + std::to_string(flow_preparations) + ".");
            }
        }

        std::cout
            << std::fixed << std::setprecision(2)
            << "Adaptive WGC timing passed: scheduled=" << scheduled_fps
            << " fps, submitted=" << submitted_fps
            << " fps, confirmed="
            << (present.statistics_available ? std::to_string(confirmed / measured_seconds) : "n/a")
            << " fps, raw=" << raw_fps
            << " fps, unique=" << unique_fps
            << " fps, duplicates=" << duplicates
            << ", capture-drops=" << drops
            << ", missed=" << missed
            << ", max-queue=" << maximum_queue_occupancy
            << ", queue-delay=" << last_queue_delay_milliseconds << " ms"
            << ", latency-avg=" << average_latency_milliseconds << " ms"
            << ", latency-max=" << maximum_latency_milliseconds << " ms"
            << ", flow-preparations=" << flow_preparations
            << ", display="
            << (display
                ? std::to_string(display->active_rate.numerator) + "/" +
                    std::to_string(display->active_rate.denominator)
                : "unknown")
            << ", display-headroom=" << (display_can_accept_target ? "yes" : "no")
            << (display && display->dynamic_refresh_enabled ? " DRR" : "")
            << ".\n";
    } catch (...) {
        source.Stop();
        throw;
    }

    source.Stop();
    return 0;
}

} // namespace osss
