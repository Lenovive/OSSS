#include "adaptive_scheduler.h"
#include "frame_rate_limits.h"
#include "pacing_mode.h"
#include "present_mode.h"
#include "platform/desktop_backend.h"
#include "platform/software_interpolator.h"
#include "ui_mask.h"
#include "window_catalog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <deque>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace osss {
namespace {

std::atomic_bool g_stop{false};

void OnSignal(int) noexcept {
    g_stop.store(true, std::memory_order_relaxed);
}

struct Options {
    int maximum_multiplier = 6;
    int buffer_milliseconds = FrameSelector::kDefaultBufferMilliseconds;
    FrameSelector::CeilingPacing ceiling_pacing = FrameSelector::CeilingPacing::even;
    PacingMode pacing_mode = PacingMode::paced;
    PresentMode present_mode = PresentMode::automatic;
    double target_fps = 0.0;
    bool target_fps_explicit = false;
    bool list_windows = false;
    bool self_test = false;
    bool help = false;
    bool fullscreen = false;
    bool stats_overlay = true;
    std::vector<UiMaskRect> ui_masks;
    bool ui_mask_auto = false;
    SoftwareInterpolator::Mode mode = SoftwareInterpolator::Mode::motion;
    std::string title;
    std::optional<WindowHandle> handle;
};

[[nodiscard]] std::string RequireValue(
    const int argc,
    char** argv,
    int& index,
    const std::string_view option) {
    if (index + 1 >= argc) {
        throw std::invalid_argument(std::string(option) + " requires a value");
    }
    return argv[++index];
}

[[nodiscard]] std::uint64_t ParseUnsigned(const std::string& value, const std::string_view option) {
    std::size_t consumed = 0;
    const std::uint64_t parsed = std::stoull(value, &consumed, 0);
    if (consumed != value.size()) {
        throw std::invalid_argument(std::string(option) + " must be an integer");
    }
    return parsed;
}

[[nodiscard]] int ParseMilliseconds(const std::string& value, const std::string_view option) {
    const std::uint64_t parsed = ParseUnsigned(value, option);
    if (parsed > static_cast<std::uint64_t>(FrameSelector::kMaximumBufferMilliseconds)) {
        throw std::invalid_argument(std::string(option) + " must be from 0 through 32");
    }
    return static_cast<int>(parsed);
}

[[nodiscard]] PacingMode ParsePacingMode(const std::string& value) {
    if (value == "unpaced" || value == "off" || value == "free") {
        return PacingMode::unpaced;
    }
    if (value == "paced" || value == "on") {
        return PacingMode::paced;
    }
    if (value == "queued" || value == "queue" || value == "render-ahead") {
        return PacingMode::queued;
    }
    throw std::invalid_argument("--pacing must be unpaced, paced, or queued");
}

[[nodiscard]] PresentMode ParsePresentMode(const std::string& value) {
    if (value == "auto" || value == "automatic") {
        return PresentMode::automatic;
    }
    if (value == "vsync" || value == "on") {
        return PresentMode::vsync;
    }
    if (value == "tearing" || value == "vrr" || value == "gsync" || value == "off") {
        return PresentMode::tearing;
    }
    throw std::invalid_argument("--present-mode must be auto, vsync, or tearing");
}

[[nodiscard]] Options ParseOptions(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--list-windows") {
            options.list_windows = true;
        } else if (argument == "--self-test") {
            options.self_test = true;
        } else if (argument == "--title") {
            options.title = RequireValue(argc, argv, index, argument);
        } else if (argument == "--hwnd") {
            options.handle = WindowHandle::FromNative(
                ParseUnsigned(RequireValue(argc, argv, index, argument), argument));
        } else if (argument == "--max-multiplier" || argument == "--multiplier" || argument == "-m") {
            options.maximum_multiplier = static_cast<int>(ParseUnsigned(
                RequireValue(argc, argv, index, argument), argument));
            if (!IsValidMultiplier(options.maximum_multiplier)) {
                throw std::invalid_argument("--max-multiplier must be from 2 through 20");
            }
        } else if (argument == "--target-fps") {
            const std::string value = RequireValue(argc, argv, index, argument);
            if (value == "auto") {
                options.target_fps_explicit = false;
            } else {
                options.target_fps = std::stod(value);
                if (!IsValidTargetFps(options.target_fps)) {
                    throw std::invalid_argument("--target-fps must be auto or from 24 through 1000");
                }
                options.target_fps_explicit = true;
            }
        } else if (argument == "--buffer") {
            options.buffer_milliseconds = ParseMilliseconds(
                RequireValue(argc, argv, index, argument), argument);
        } else if (argument == "--ceiling-pacing") {
            const std::string value = RequireValue(argc, argv, index, argument);
            if (value == "even") {
                options.ceiling_pacing = FrameSelector::CeilingPacing::even;
            } else if (value == "spread") {
                options.ceiling_pacing = FrameSelector::CeilingPacing::spread;
            } else {
                throw std::invalid_argument("--ceiling-pacing must be even or spread");
            }
        } else if (argument == "--pacing") {
            options.pacing_mode = ParsePacingMode(
                RequireValue(argc, argv, index, argument));
        } else if (argument == "--present-mode") {
            options.present_mode = ParsePresentMode(
                RequireValue(argc, argv, index, argument));
        } else if (argument == "--stats-overlay") {
            const std::string value = RequireValue(argc, argv, index, argument);
            if (value == "on") {
                options.stats_overlay = true;
            } else if (value == "off") {
                options.stats_overlay = false;
            } else {
                throw std::invalid_argument("--stats-overlay must be on or off");
            }
        } else if (argument == "--ui-mask") {
            const UiMaskParseResult parsed = ParseUiMaskRects(
                RequireValue(argc, argv, index, argument));
            if (!parsed.Ok()) {
                throw std::invalid_argument("--ui-mask: " + parsed.error);
            }
            options.ui_masks.insert(options.ui_masks.end(), parsed.rects.begin(), parsed.rects.end());
        } else if (argument == "--ui-mask-auto") {
            const std::string value = RequireValue(argc, argv, index, argument);
            if (value == "on") {
                options.ui_mask_auto = true;
            } else if (value == "off") {
                options.ui_mask_auto = false;
            } else {
                throw std::invalid_argument("--ui-mask-auto must be on or off");
            }
        } else if (argument == "--interpolator") {
            const std::string value = RequireValue(argc, argv, index, argument);
            if (value == "blend") {
                options.mode = SoftwareInterpolator::Mode::blend;
            } else if (value == "motion") {
                options.mode = SoftwareInterpolator::Mode::motion;
            } else {
                throw std::invalid_argument("--interpolator must be motion or blend");
            }
        } else if (argument == "--output-mode") {
            const std::string value = RequireValue(argc, argv, index, argument);
            if (value == "fullscreen") {
                options.fullscreen = true;
            } else if (value == "overlay") {
                options.fullscreen = false;
            } else {
                throw std::invalid_argument("--output-mode must be overlay or fullscreen");
            }
        } else if (argument == "--flow-scale" || argument == "--performance-mode" ||
                   argument == "--temporal-prior" || argument == "--upscale" ||
                   argument == "--sharpness" || argument == "--debug-view" ||
                   argument == "--profile" || argument == "--save-profile") {
            static_cast<void>(RequireValue(argc, argv, index, argument));
            throw std::invalid_argument(argument + " is only available on the Windows GPU backend");
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    if (options.handle && !options.title.empty()) {
        throw std::invalid_argument("Choose either --hwnd or --title, not both");
    }
    return options;
}

void PrintUsage() {
    std::cout
        << "OSSS - Open Source Super Scaler (portable desktop backend)\n\n"
        << "Usage:\n"
        << "  osss --list-windows\n"
        << "  osss --title <window title or executable fragment> [options]\n"
        << "  osss --hwnd <native window id> [options]\n"
        << "  osss --self-test\n\n"
        << "Options:\n"
        << "  --max-multiplier 2..20   maximum generated/source ratio (default 6)\n"
        << "  --target-fps auto|24..1000\n"
        << "  --buffer 0..32           adaptive queue jitter floor in milliseconds\n"
        << "  --ceiling-pacing even|spread\n"
        << "  --pacing unpaced|paced|queued\n"
        << "  --present-mode auto|vsync|tearing (backend advisory)\n"
        << "  --interpolator motion|blend\n"
        << "  --stats-overlay on|off   periodic terminal telemetry\n"
        << "  --ui-mask <rects>        static regions kept on the newest frame\n"
        << "  --ui-mask-auto on|off    reserved for the dense GPU backend\n"
        << "  --output-mode overlay|fullscreen\n\n"
        << "The portable backend uses CoreGraphics on macOS and X11 on Linux.\n"
        << "Press Ctrl+C to stop a live session.\n";
}

[[nodiscard]] PixelFrame MakePattern(
    const std::uint32_t width,
    const std::uint32_t height,
    const int box_x) {
    PixelFrame frame;
    frame.Reset(width, height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const bool box = static_cast<int>(x) >= box_x && static_cast<int>(x) < box_x + 16 &&
                y >= 20 && y < 44;
            frame.pixels[static_cast<std::size_t>(y) * width + x] =
                box ? 0xFF40D0FFU : 0xFF101820U;
        }
    }
    return frame;
}

[[nodiscard]] int RunSelfTest() {
    const PixelFrame previous = MakePattern(96, 64, 20);
    const PixelFrame current = MakePattern(96, 64, 28);
    SoftwareInterpolator interpolator;
    if (!interpolator.Prepare(previous, current)) {
        throw std::runtime_error(interpolator.LastError());
    }
    if (std::abs(interpolator.MotionX() - 8) > 2 || std::abs(interpolator.MotionY()) > 2) {
        throw std::runtime_error("software motion self-test did not recover the known translation");
    }
    const PixelFrame midpoint = interpolator.Render(0.5F);
    const PixelFrame expected = MakePattern(96, 64, 24);
    std::size_t matching = 0;
    for (std::size_t index = 0; index < midpoint.pixels.size(); ++index) {
        if (midpoint.pixels[index] == expected.pixels[index]) {
            ++matching;
        }
    }
    if (matching < midpoint.pixels.size() * 9U / 10U) {
        throw std::runtime_error("software motion self-test midpoint is not reconstructed");
    }

    PixelFrame white;
    white.Reset(previous.width, previous.height);
    std::fill(white.pixels.begin(), white.pixels.end(), 0xFFFFFFFFU);
    SoftwareInterpolator cut;
    if (!cut.Prepare(previous, white)) {
        throw std::runtime_error(cut.LastError());
    }
    const PixelFrame cut_frame = cut.Render(0.75F);
    if (cut_frame.pixels != white.pixels) {
        throw std::runtime_error("software scene-cut self-test did not select the newest frame");
    }
    std::cout << "Portable backend self-test passed: translation="
        << interpolator.MotionX() << "," << interpolator.MotionY()
        << ", matching=" << matching << "/" << midpoint.pixels.size() << ".\n";
    return 0;
}

struct StoredFrame {
    std::uint64_t unique_sequence = 0;
    PixelFrame frame;
};

[[nodiscard]] StoredFrame* FindStored(
    std::deque<StoredFrame>& frames,
    const std::uint64_t sequence) {
    const auto iterator = std::find_if(frames.begin(), frames.end(), [sequence](const StoredFrame& frame) {
        return frame.unique_sequence == sequence;
    });
    return iterator == frames.end() ? nullptr : &*iterator;
}

[[nodiscard]] bool SamePixels(const PixelFrame& left, const PixelFrame& right) {
    return left.width == right.width && left.height == right.height && left.pixels == right.pixels;
}

int RunLive(const Options& options) {
    std::vector<WindowEntry> matches;
    WindowHandle target;
    if (options.handle) {
        target = *options.handle;
    } else {
        matches = FindWindowsMatching(options.title);
        if (matches.size() != 1) {
            std::ostringstream message;
            message << "--title must resolve to exactly one window (matched " << matches.size() << ")";
            throw std::runtime_error(message.str());
        }
        target = matches.front().handle;
    }

    const auto display = WindowDisplayRefreshInfo(target);
    const double display_fps = display ? display->active_rate.AsDouble() : 60.0;
    const double target_fps = options.target_fps_explicit
        ? options.target_fps
        : std::clamp(display_fps * static_cast<double>(options.maximum_multiplier), 24.0, 1000.0);
    const FrameRate target_rate = FrameRate::FromFps(target_fps);

    DesktopCapture capture;
    std::string error;
    if (!capture.Start(target, error)) {
        throw std::runtime_error(error);
    }
    DesktopPresenter presenter;
    if (!presenter.Start(capture.Bounds(), options.fullscreen, error)) {
        throw std::runtime_error(error);
    }

    FrameSelector selector(
        options.maximum_multiplier,
        std::chrono::milliseconds(options.buffer_milliseconds),
        options.ceiling_pacing);
    SourceTimeline timeline;
    OutputClock clock(target_rate);
    const auto start = OutputClock::Clock::now();
    const bool clock_owned = PacingModeUsesOutputClock(options.pacing_mode);
    if (clock_owned) {
        clock.Start(start);
        if (PacingModeRendersAhead(options.pacing_mode)) {
            selector.SetLookahead(clock.DeadlineForSlot(1) - clock.DeadlineForSlot(0));
        }
    }
    SoftwareInterpolator interpolator;
    interpolator.SetMode(options.mode);
    std::uint32_t mask_width = 0;
    std::uint32_t mask_height = 0;
    if (options.ui_mask_auto) {
        std::cout << "Note: --ui-mask-auto is only available on the dense GPU "
            "backend; explicit --ui-mask regions are active.\n";
    }
    std::deque<StoredFrame> stored;
    PixelFrame last_captured;
    std::uint64_t raw_sequence = 0;
    auto next_capture = start;
    const auto source_period = std::chrono::microseconds(16667);
    std::uint64_t presented = 0;
    std::uint64_t generated = 0;
    auto last_report = start;

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::cout << std::fixed << std::setprecision(2)
        << "Portable session started: source=" << display_fps
        << " fps, target=" << target_fps << " fps, pacing="
        << PacingModeName(options.pacing_mode) << ", present="
        << PresentModeName(options.present_mode) << ".\n";
    if (options.present_mode != PresentMode::automatic) {
        std::cout << "Note: the portable presenter does not expose a native "
            "vsync/tearing switch; the requested present mode is advisory.\n";
    }
    if (options.pacing_mode == PacingMode::queued) {
        std::cout << "Note: portable queued pacing applies scheduler lookahead; "
            "native swap-chain frame latency is unavailable.\n";
    }
    while (!g_stop.load(std::memory_order_relaxed)) {
        const auto now = OutputClock::Clock::now();
        if (now >= next_capture) {
            PixelFrame captured;
            if (!capture.Read(captured, error)) {
                throw std::runtime_error(error);
            }
            const auto captured_at = OutputClock::Clock::now();
            if (!presenter.UpdateBounds(capture.Bounds(), error)) {
                throw std::runtime_error(error);
            }
            if (last_captured.IsValid() &&
                (last_captured.width != captured.width || last_captured.height != captured.height)) {
                stored.clear();
            }
            if (mask_width != captured.width || mask_height != captured.height) {
                mask_width = captured.width;
                mask_height = captured.height;
                if (options.ui_masks.empty()) {
                    interpolator.ClearMask();
                } else {
                    interpolator.SetMask(RasterizeUiMask(
                        options.ui_masks, captured.width, captured.height));
                }
            }
            const bool duplicate = last_captured.IsValid() && SamePixels(last_captured, captured);
            ++raw_sequence;
            const auto unique = timeline.Ingest(
                raw_sequence,
                captured.media_time,
                captured_at,
                captured.width,
                captured.height,
                duplicate,
                captured_at);
            if (unique) {
                stored.push_back(StoredFrame{*unique, std::move(captured)});
                while (stored.size() > timeline.Frames().size() + 1U) {
                    stored.pop_front();
                }
            }
            last_captured = stored.empty() ? PixelFrame{} : stored.back().frame;
            do {
                next_capture += source_period;
            } while (next_capture <= captured_at);
        }

        std::optional<FrameSelection> due_selection;
        if (clock_owned) {
            if (const auto deadline = clock.TakeLatestDue(now)) {
                due_selection = selector.Select(*deadline, target_rate, timeline);
            }
        } else {
            // Unpaced has no deadline grid. It still uses the selector's media
            // queue and multiplier policy, but asks what is ready at this
            // instant and presents as soon as the software presenter accepts it.
            due_selection = selector.SelectNow(now, target_rate, timeline);
        }
        if (due_selection) {
            const FrameSelection selection = *due_selection;
            if (selection.submit) {
                PixelFrame output;
                if (selection.mode == FrameSelectionMode::interpolate) {
                    auto* previous = FindStored(stored, selection.previous_unique_sequence);
                    auto* current = FindStored(stored, selection.current_unique_sequence);
                    if (previous && current && interpolator.Prepare(previous->frame, current->frame)) {
                        output = interpolator.Render(selection.alpha);
                        ++generated;
                    }
                }
                if (!output.IsValid()) {
                    if (auto* current = FindStored(stored, selection.current_unique_sequence)) {
                        output = current->frame;
                    } else if (!stored.empty()) {
                        output = stored.back().frame;
                    }
                }
                if (output.IsValid() && presenter.Present(output.pixels, output.width, output.height, error)) {
                    ++presented;
                } else if (!error.empty()) {
                    throw std::runtime_error(error);
                }
            }
        }
        presenter.Pump();
        if (options.stats_overlay && now - last_report >= std::chrono::seconds(2)) {
            std::cout << "presented=" << presented << " generated=" << generated
                << " unique=" << timeline.UniqueFrameCount()
                << " duplicates=" << timeline.DuplicateFrameCount()
                << " missed=" << clock.MissedDeadlineCount() << ".\n";
            last_report = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    presenter.Stop();
    capture.Stop();
    std::cout << "Portable session stopped: presented=" << presented
        << ", generated=" << generated << ".\n";
    return 0;
}

} // namespace
} // namespace osss

int main(int argc, char** argv) {
    try {
        const osss::Options options = osss::ParseOptions(argc, argv);
        if (options.help || argc == 1) {
            osss::PrintUsage();
            return 0;
        }
        if (options.list_windows) {
            for (const osss::WindowEntry& window : osss::ListCapturableWindows()) {
                std::cout << window.handle.Native() << "\t" << window.process_name
                    << "\t" << window.title << "\n";
            }
            return 0;
        }
        if (options.self_test) {
            return osss::RunSelfTest();
        }
        if (options.title.empty() && !options.handle) {
            osss::PrintUsage();
            return 2;
        }
        return osss::RunLive(options);
    } catch (const std::exception& error) {
        std::cerr << "osss: " << error.what() << "\n";
        return 1;
    }
}
