#include "adaptive_scheduler.h"
#include "capture_session.h"
#include "capture_smoke.h"
#include "shader_cache.h"
#include "dpi_awareness.h"
#include "flow_scale.h"
#include "app_profile.h"
#include "debug_view.h"
#include "output_mode.h"
#include "pacing_mode.h"
#include "upscaler.h"
#include "frame_rate_limits.h"
#include "renderer.h"
#include "stats_overlay.h"
#include "ui_mask.h"
#include "window_catalog.h"
#include "platform/unicode.h"

#include <windows.h>
#include <avrt.h>

#include <shellapi.h>
#include <winrt/base.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

// The core's IntRect and Win32's RECT describe the same rectangle differently
// (top-left + size versus top-left + bottom-right); this conversion lives at
// the window boundary, where the two types meet.
[[nodiscard]] RECT ToRect(const osss::IntRect& rect) {
    return RECT{rect.x, rect.y, rect.x + rect.width, rect.y + rect.height};
}

struct Options {
    int max_multiplier = 6;
    std::chrono::milliseconds buffer_floor{
        osss::FrameSelector::kDefaultBufferMilliseconds};
    osss::FrameSelector::CeilingPacing ceiling_pacing =
        osss::FrameSelector::CeilingPacing::even;
    osss::PresentMode present_mode = osss::PresentMode::automatic;
    osss::PacingMode pacing_mode = osss::PacingMode::paced;
    std::optional<double> target_fps;
    bool help = false;
    bool list_windows = false;
    bool self_test = false;
    bool warm_shader_cache = false;
    bool capture_self_test = false;
    bool adaptive_capture_self_test = false;
    bool motion_interpolation = true;
    bool stats_overlay = true;
    std::vector<osss::UiMaskRect> ui_mask;
    bool ui_mask_auto = false;
    osss::FlowScale flow_scale = osss::FlowScale::automatic;
    bool performance_mode = false;
    bool temporal_prior = true;
    osss::DebugView debug_view = osss::DebugView::off;
    std::optional<std::wstring> profile;
    std::optional<std::wstring> save_profile;
    osss::OutputMode output_mode = osss::OutputMode::overlay;
    osss::UpscaleMode upscale_mode = osss::UpscaleMode::automatic;
    float upscale_sharpness = osss::kDefaultSharpness;
    std::optional<HWND> window;
    std::optional<std::wstring> title;
    std::optional<std::wstring> stop_event;
};

class ScopedHandle {
public:
    ScopedHandle() = default;
    ~ScopedHandle() {
        Reset();
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    void Reset(HANDLE handle = nullptr) noexcept {
        if (handle_) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

    [[nodiscard]] HANDLE Get() const noexcept {
        return handle_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

private:
    HANDLE handle_ = nullptr;
};

enum class WaitReason {
    deadline,
    capture,
    // The swap chain's frame-latency waitable object signalled: a back buffer
    // is free. Waiting on it *consumed* that signal, so the caller now holds
    // the slot and must tell the renderer (MarkPresentationSlotAcquired).
    slot,
    stop,
    messages,
};

class DeadlineWaiter {
public:
    DeadlineWaiter() {
        HANDLE timer = CreateWaitableTimerExW(
            nullptr,
            nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_ALL_ACCESS);
        if (!timer && GetLastError() == ERROR_INVALID_PARAMETER) {
            timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        }
        if (!timer) {
            winrt::throw_last_error();
        }
        timer_.Reset(timer);
    }

    // `slot_event` is optional and is the swap chain's frame-latency waitable
    // object. Pass it only when the caller wants to be woken by a free back
    // buffer *and does not already hold one*: the object is a semaphore, so a
    // successful wait on it takes the slot, and passing it while a slot is
    // already held would spin.
    [[nodiscard]] WaitReason WaitUntil(
        const std::chrono::steady_clock::time_point deadline,
        const HANDLE capture_event,
        const HANDLE stop_event,
        const HANDLE slot_event = nullptr) {
        using HundredNanoseconds =
            std::chrono::duration<LONGLONG, std::ratio<1, 10'000'000>>;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return WaitReason::deadline;
        }

        const LONGLONG remaining = std::max<LONGLONG>(
            1,
            std::chrono::duration_cast<HundredNanoseconds>(deadline - now).count());
        LARGE_INTEGER due_time{};
        due_time.QuadPart = -remaining;
        if (!SetWaitableTimer(timer_.Get(), &due_time, 0, nullptr, nullptr, FALSE)) {
            winrt::throw_last_error();
        }

        HANDLE handles[4] = {timer_.Get(), nullptr, nullptr, nullptr};
        DWORD handle_count = 1;
        DWORD capture_index = MAXDWORD;
        DWORD stop_index = MAXDWORD;
        DWORD slot_index = MAXDWORD;
        if (capture_event) {
            capture_index = handle_count;
            handles[handle_count++] = capture_event;
        }
        if (stop_event) {
            stop_index = handle_count;
            handles[handle_count++] = stop_event;
        }
        if (slot_event) {
            slot_index = handle_count;
            handles[handle_count++] = slot_event;
        }

        const DWORD result = MsgWaitForMultipleObjectsEx(
            handle_count,
            handles,
            INFINITE,
            QS_ALLINPUT,
            MWMO_ALERTABLE | MWMO_INPUTAVAILABLE);
        CancelWaitableTimer(timer_.Get());
        if (result == WAIT_OBJECT_0) {
            return WaitReason::deadline;
        }
        if (capture_index != MAXDWORD && result == WAIT_OBJECT_0 + capture_index) {
            return WaitReason::capture;
        }
        if (stop_index != MAXDWORD && result == WAIT_OBJECT_0 + stop_index) {
            return WaitReason::stop;
        }
        if (slot_index != MAXDWORD && result == WAIT_OBJECT_0 + slot_index) {
            return WaitReason::slot;
        }
        if (result == WAIT_OBJECT_0 + handle_count || result == WAIT_IO_COMPLETION) {
            return WaitReason::messages;
        }
        winrt::throw_last_error();
    }

private:
    ScopedHandle timer_;
};

class ScopedPresentationPriority {
public:
    ScopedPresentationPriority() {
        mmcss_handle_ = AvSetMmThreadCharacteristicsW(L"Games", &mmcss_task_index_);
        if (!mmcss_handle_) {
            previous_priority_ = GetThreadPriority(GetCurrentThread());
            if (previous_priority_ != THREAD_PRIORITY_ERROR_RETURN) {
                elevated_ = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != FALSE;
            }
        }
    }

    ~ScopedPresentationPriority() {
        if (mmcss_handle_) {
            AvRevertMmThreadCharacteristics(mmcss_handle_);
        } else if (elevated_ && previous_priority_ != THREAD_PRIORITY_ERROR_RETURN) {
            SetThreadPriority(GetCurrentThread(), previous_priority_);
        }
    }

    ScopedPresentationPriority(const ScopedPresentationPriority&) = delete;
    ScopedPresentationPriority& operator=(const ScopedPresentationPriority&) = delete;

    [[nodiscard]] bool UsingMmcss() const noexcept {
        return mmcss_handle_ != nullptr;
    }

private:
    HANDLE mmcss_handle_ = nullptr;
    DWORD mmcss_task_index_ = 0;
    int previous_priority_ = THREAD_PRIORITY_ERROR_RETURN;
    bool elevated_ = false;
};

struct ResolvedTarget {
    osss::FrameRate rate{};
    std::optional<osss::DisplayRefreshInfo> display;
    bool manual = false;
};

void PrintUsage() {
    std::wcout
        << L"OSSS - Open Source Super Scaler (frame generation prototype)\n\n"
        << L"Usage:\n"
        << L"  osss --list-windows\n"
        << L"  osss --title <title or executable fragment> [--target-fps auto|24..1000]\n"
        << L"  osss --hwnd <decimal or 0x...> [--target-fps auto|24..1000]\n"
        << L"       [--max-multiplier " << osss::kMinimumMultiplier << L".."
            << osss::kMaximumMultiplier << L"]   (aliases: --multiplier, -m)\n"
        << L"       [--buffer 0..32] (adaptive queue jitter floor in milliseconds)\n"
        << L"       [--ceiling-pacing even|spread] (even is the default)\n"
        << L"       [--present-mode auto|vsync|tearing] (auto is the default)\n"
        << L"       [--pacing unpaced|paced|queued] (paced is the default)\n"
        << L"       [--interpolator motion|blend] [--stats-overlay on|off]\n"
        << L"       [--ui-mask \"left,top,right,bottom[px][; ...]\"] [--ui-mask-auto on|off]\n"
        << L"       [--flow-scale auto|quality|performance|ultra-performance]\n"
        << L"       [--performance-mode on|off] (off is the default)\n"
        << L"       [--temporal-prior on|off] (on is the default)\n"
        << L"       [--output-mode overlay|fullscreen] (overlay is the default)\n"
        << L"       [--upscale off|auto|always] [--sharpness 0..1]\n"
        << L"       [--debug-view off|flow|confidence|fallback]\n"
        << L"       [--profile auto|<name.exe>] [--save-profile <name.exe>]\n"
        << L"  osss --self-test\n\n"
        << L"  osss --capture-self-test\n\n"
        << L"  osss --adaptive-capture-self-test\n\n"
        << L"  osss --warm-shader-cache\n\n"
        << L"--title matches a case-insensitive fragment of either the window title or the\n"
        << L"executable file name, so --title vlc finds \"movie.mkv - VLC media player\".\n"
        << L"It must resolve to exactly one window; otherwise pass --hwnd.\n\n"
        << L"--ui-mask lists static UI/HUD rectangles that are never interpolated: they\n"
        << L"always show the newest real frame and do not steer optical flow around them.\n"
        << L"Values are fractions of the captured frame (0-1) or source pixels when the\n"
        << L"region ends in px (or any value exceeds 1). Separate regions with ';'.\n"
        << L"Example: --ui-mask \"0,0,0.22,0.18; 1560,940,1920,1080px\"\n"
        << L"--ui-mask-auto on additionally discovers static overlays on its own: regions\n"
        << L"that hold still while the scene around them moves are masked after about ten\n"
        << L"source pairs and released within about three once they start moving. It is off\n"
        << L"by default and combines with any --ui-mask rectangles.\n\n"
        << L"--flow-scale sets the resolution motion estimation runs at, as a divisor of\n"
        << L"the source size. Motion is low-frequency, so a coarser grid costs much less\n"
        << L"and loses little on large objects; what it loses is thin structures, sharp\n"
        << L"motion boundaries, and small fast objects. Lower it when the interpolator is\n"
        << L"taking GPU time the game needs back -- that shows up as a realized multiplier\n"
        << L"below the one you asked for, because a starved source produces fewer frames\n"
        << L"to interpolate between.\n"
        << L"  auto               4 source pixels per flow cell up to 1440p, 8 above it.\n"
        << L"                     The default, and what every published measurement in\n"
        << L"                     this repo was taken with.\n"
        << L"  quality            4 everywhere. Finer than auto above 1440p, and dearer.\n"
        << L"  performance        8 everywhere. Coarser than auto at 1440p and below.\n"
        << L"  ultra-performance  16 everywhere. The cheapest flow this pipeline has.\n"
        << L"None of these changes how far the search reaches -- see below. They trade\n"
        << L"detail against GPU time only.\n\n"
        << L"--performance-mode on estimates motion more cheaply at the same flow\n"
        << L"resolution: it halves the coarse search radius, and skips the second local\n"
        << L"search that resolves periodic detail landing in the wrong period. Repeating\n"
        << L"fine texture is what regresses first.\n\n"
        << L"How far the search reaches is a target in source pixels, and it is held\n"
        << L"there against the two things that would otherwise move it.\n"
        << L"The first is the source rate: the motion the estimator must find is a\n"
        << L"velocity times the frame period, so the search scales with the measured\n"
        << L"period. 64 source pixels at 60 FPS, and the same camera speed stays inside\n"
        << L"the search at 30 and at 120.\n"
        << L"The second is --flow-scale. Reach used to be counted in flow cells, so\n"
        << L"asking for a finer grid silently halved it -- quality reached 32 source\n"
        << L"pixels where auto above 1440p reached 64. It no longer does: every\n"
        << L"--flow-scale reaches the same distance, and the setting buys detail\n"
        << L"instead of spending range. A finer grid pays for that in GPU time, since\n"
        << L"holding reach costs a wider search on a grid that already has more cells.\n"
        << L"The per-second telemetry line reports the result as flow-search, in source\n"
        << L"pixels: motion faster than that per source frame cannot be estimated at all\n"
        << L"and falls back to a crossfade, which is what --debug-view fallback shows.\n\n"
        << L"--temporal-prior on (the default) also seeds each pair's search with the\n"
        << L"previous pair's motion when the two pairs are consecutive, so a pan that\n"
        << L"accelerates out past the search radius can be followed one flow cell per\n"
        << L"source frame instead of being lost the moment it leaves. It is an extra\n"
        << L"candidate, never a bias: where the pixels do not support it, it loses.\n"
        << L"Costs about a tenth of the coarse search. Measured on the quality bench it\n"
        << L"pays off in performance mode (about 4.5 dB on the first pan past the\n"
        << L"radius) and is neutral in quality mode; off is for A/B comparison.\n\n"
        << L"--output-mode decides whether the display can follow OSSS or only DWM.\n"
        << L"  overlay     a click-through surface sized to the target window. Layered,\n"
        << L"              which is what lets clicks reach the game underneath -- and\n"
        << L"              which also means DWM always composes it, so no variable-\n"
        << L"              refresh display can follow it. The default.\n"
        << L"  fullscreen  an opaque window covering the monitor, which DWM can promote\n"
        << L"              to independent flip. This is what G-Sync and FreeSync need in\n"
        << L"              order to follow the output clock. Pair it with\n"
        << L"              --present-mode tearing. It gives up click-through: the game\n"
        << L"              keeps keyboard focus and raw-input mice still work, but a\n"
        << L"              title needing ordinary mouse messages will not get them. The\n"
        << L"              FPS overlay is disabled in this mode because a second topmost\n"
        << L"              window would silently demote the output back to composed.\n\n"
        << L"--upscale spatially upscales the finished frame when the output is larger\n"
        << L"than the captured source, which is the fullscreen case where the target\n"
        << L"window is smaller than the monitor. It runs on the fused frame, never on\n"
        << L"the source: optical flow keeps estimating on native captured pixels, which\n"
        << L"is both cheaper and more accurate than estimating on invented ones.\n"
        << L"An edge-directed 12-tap pass is followed by a contrast-limited sharpener;\n"
        << L"--sharpness sets the latter from 0 through 1, defaulting to 0.35. auto is\n"
        << L"the default and is a no-op at 1:1.\n\n"
        << L"--debug-view replaces the output with a picture of the interpolator's own\n"
        << L"internals. Every motion defect found in this project so far was diagnosed\n"
        << L"by hand-editing the fusion shader to emit an intermediate as colour, so it\n"
        << L"is a flag now.\n"
        << L"  flow        direction as hue, magnitude as brightness. Grey is no motion;\n"
        << L"              speckle means the estimator disagrees with itself.\n"
        << L"  confidence  white where the warps were trusted, black where the crossfade\n"
        << L"              fallback took the pixel. A mostly black frame means the\n"
        << L"              interpolator has silently become a crossfade.\n"
        << L"  fallback    which safeguard claimed each pixel: red below the confidence\n"
        << L"              floor, green UI-masked, blue static-pixel protection.\n\n"
        << L"--profile applies the arguments stored for <name.exe> before the ones on\n"
        << L"this command line, so anything given explicitly here still wins. Pass\n"
        << L"--profile auto to key off the target's own executable instead of naming\n"
        << L"one; having no profile for it is then the ordinary case, not an error.\n"
        << L"this command line, so anything given explicitly here still wins. A profile\n"
        << L"is stored as the command line you would have typed, which means every flag\n"
        << L"is validated by the same branch that validates it here and no setting can\n"
        << L"drift between the two.\n"
        << L"--save-profile writes the rest of this command line to <name.exe> and exits\n"
        << L"without capturing anything, replacing any existing section for it.\n"
        << L"Profiles live in %LOCALAPPDATA%\\OSSS\\profiles.txt and can be edited by hand.\n\n"
        << L"--present-mode chooses how a finished frame reaches the display, which is\n"
        << L"what frame pacing mostly consists of:\n"
        << L"  vsync    every present waits for a vblank. Tear-free, but it quantizes\n"
        << L"           output to the refresh period, so a target rate that is not a\n"
        << L"           divisor of the refresh rate beats against it -- a 120 FPS target\n"
        << L"           on a 144 Hz panel alternates 6.9 ms and 13.9 ms frames forever.\n"
        << L"  tearing  the output clock alone decides when a frame appears, so any\n"
        << L"           target rate is paced exactly. Required for G-Sync and FreeSync:\n"
        << L"           with variable refresh on, the display follows the presents and\n"
        << L"           nothing tears. With variable refresh off, it tears.\n"
        << L"  auto     tearing wherever DXGI reports support for it, else vsync. The\n"
        << L"           startup banner prints which one was actually selected.\n\n"
        << L"--pacing chooses when, relative to the output clock, a frame is rendered\n"
        << L"and handed over. --present-mode decides how a present reaches the display;\n"
        << L"this decides whether the loop waits for a deadline at all, and how much work\n"
        << L"it may have in flight ahead of one. Latency comes almost entirely from the\n"
        << L"media queue (one source period plus --buffer), which is the same in every\n"
        << L"mode; what changes is the cadence and where the render sits.\n"
        << L"  unpaced  no deadline grid. Whenever a back buffer is free the loop selects\n"
        << L"           for now, renders, and presents. Cadence is whatever the swap\n"
        << L"           chain permits -- refresh-locked under vsync, composition-limited\n"
        << L"           for the overlay under tearing, free-running only for fullscreen\n"
        << L"           independent flip. Nothing is ever late and nothing is evenly\n"
        << L"           spaced: the first slot of every source pair carries the flow\n"
        << L"           pass. --target-fps and --max-multiplier do not limit cadence in\n"
        << L"           this mode. A measurement floor and a parity mode, not a default.\n"
        << L"  paced    a rational output clock owns the timeline; each slot is selected,\n"
        << L"           rendered, and presented at its deadline with maximum frame\n"
        << L"           latency one, so no queued frame ever adds hidden latency. A slot\n"
        << L"           whose back buffer is not free is dropped and counted as missed.\n"
        << L"           The default, and what every measurement in this repo used.\n"
        << L"  queued   the same clock, rendering one slot ahead: slot k+1 is selected\n"
        << L"           and rendered as soon as slot k is handed over, and its own\n"
        << L"           deadline costs only a Present. Maximum frame latency two and a\n"
        << L"           third back buffer make that legal. The flow pass leaves the\n"
        << L"           critical path, so a slow pair costs nothing unless it exceeds a\n"
        << L"           whole slot; the price is exactly one target slot of extra media\n"
        << L"           delay (about 4 ms at 240 FPS), taken on the media clock rather\n"
        << L"           than by presenting early -- frames still hand over on time.\n\n"
        << L"--warm-shader-cache compiles the motion shaders into the per-user bytecode\n"
        << L"cache and exits. The first compile takes several seconds; every later start\n"
        << L"reads the cache instead. The launcher runs this in the background on open,\n"
        << L"so a session started from it never waits for a compile.\n\n"
        << L"The target must be visible and not minimized. Target FPS defaults to the\n"
        << L"target display refresh rate. Native capture runs independently; OSSS adds\n"
        << L"only enough interpolation to reach the target, up to the maximum multiplier.\n"
        << L"The stop hotkey is Ctrl+Alt+F12 when that chord is free; if another\n"
        << L"process already owns it OSSS falls back and prints the one it got in\n"
        << L"the startup banner. Closing the target window also stops a session.\n"
        << L"Ctrl+Alt+F11 toggles frame generation off and on without ending the\n"
        << L"session: capture, the overlay, and the output window all stay up, and\n"
        << L"the display sees native frames only. It walks the same kind of fallback\n"
        << L"list and is printed beside the stop chord. Use it to compare generated\n"
        << L"against native on one scene without relaunching and losing the scene.\n"
        << L"The overlay hides itself whenever the target is minimized or leaves the\n"
           L"foreground, and comes back when the target does.\n";
}

std::wstring RequireValue(const int argc, wchar_t** argv, int& index, const wchar_t* option) {
    if (index + 1 >= argc) {
        throw std::invalid_argument(winrt::to_string(std::wstring(option) + L" requires a value."));
    }
    return argv[++index];
}

osss::PresentMode RequirePresentMode(const std::wstring& value) {
    if (const auto mode = osss::ParsePresentMode(value)) {
        return *mode;
    }
    throw std::invalid_argument("--present-mode must be auto, vsync, or tearing.");
}

osss::PacingMode RequirePacingMode(const std::wstring& value) {
    if (const auto mode = osss::ParsePacingMode(value)) {
        return *mode;
    }
    throw std::invalid_argument("--pacing must be unpaced, paced, or queued.");
}

double ParseTargetFps(const std::wstring& value) {
    try {
        std::size_t consumed = 0;
        const double parsed = std::stod(value, &consumed);
        if (consumed == value.size() && osss::IsValidTargetFps(parsed)) {
            return parsed;
        }
    } catch (const std::exception&) {
    }
    throw std::invalid_argument(
        "--target-fps must be auto or a number from 24 through 1000.");
}

int ParseMaxMultiplier(const std::wstring& value) {
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(value, &consumed);
        if (consumed == value.size() && osss::IsValidMultiplier(parsed)) {
            return parsed;
        }
    } catch (const std::exception&) {
    }
    throw std::invalid_argument(
        "--max-multiplier must be an integer from " +
            std::to_string(osss::kMinimumMultiplier) + " through " +
            std::to_string(osss::kMaximumMultiplier) + ".");
}

std::chrono::milliseconds ParseBufferMilliseconds(const std::wstring& value) {
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(value, &consumed);
        if (consumed == value.size() &&
            parsed >= osss::FrameSelector::kMinimumBufferMilliseconds &&
            parsed <= osss::FrameSelector::kMaximumBufferMilliseconds) {
            return std::chrono::milliseconds(parsed);
        }
    } catch (const std::exception&) {
    }
    throw std::invalid_argument("--buffer must be an integer from 0 through 32 milliseconds.");
}

osss::FrameSelector::CeilingPacing ParseCeilingPacing(const std::wstring& value) {
    if (value == L"even") {
        return osss::FrameSelector::CeilingPacing::even;
    }
    if (value == L"spread") {
        return osss::FrameSelector::CeilingPacing::spread;
    }
    throw std::invalid_argument("--ceiling-pacing must be even or spread.");
}

Options ParseOptions(const int argc, wchar_t** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument = argv[index];
        if (argument == L"--help" || argument == L"-h") {
            options.help = true;
        } else if (argument == L"--list-windows") {
            options.list_windows = true;
        } else if (argument == L"--warm-shader-cache") {
            options.warm_shader_cache = true;
        } else if (argument == L"--self-test") {
            options.self_test = true;
        } else if (argument == L"--capture-self-test") {
            options.capture_self_test = true;
        } else if (argument == L"--adaptive-capture-self-test") {
            options.adaptive_capture_self_test = true;
        } else if (argument == L"--interpolator") {
            const std::wstring value = RequireValue(argc, argv, index, L"--interpolator");
            if (value == L"motion") {
                options.motion_interpolation = true;
            } else if (value == L"blend") {
                options.motion_interpolation = false;
            } else {
                throw std::invalid_argument("--interpolator must be motion or blend.");
            }
        } else if (argument == L"--ui-mask") {
            const std::wstring value = RequireValue(argc, argv, index, L"--ui-mask");
            auto parsed = osss::ParseUiMaskRects(value);
            if (!parsed.Ok()) {
                throw std::invalid_argument("--ui-mask: " + parsed.error);
            }
            options.ui_mask.insert(
                options.ui_mask.end(),
                parsed.rects.begin(),
                parsed.rects.end());
        } else if (argument == L"--ui-mask-auto") {
            const std::wstring value = RequireValue(argc, argv, index, L"--ui-mask-auto");
            if (value == L"on") {
                options.ui_mask_auto = true;
            } else if (value == L"off") {
                options.ui_mask_auto = false;
            } else {
                throw std::invalid_argument("--ui-mask-auto must be on or off.");
            }
        } else if (argument == L"--profile") {
            options.profile = RequireValue(argc, argv, index, L"--profile");
            if (options.profile->empty()) {
                throw std::invalid_argument(
                    "--profile must name an executable, or be auto.");
            }
        } else if (argument == L"--save-profile") {
            options.save_profile = RequireValue(argc, argv, index, L"--save-profile");
            if (options.save_profile->empty()) {
                throw std::invalid_argument("--save-profile must name an executable.");
            }
        } else if (argument == L"--debug-view") {
            const std::wstring value = RequireValue(argc, argv, index, L"--debug-view");
            const auto parsed = osss::ParseDebugView(value);
            if (!parsed) {
                throw std::invalid_argument(
                    "--debug-view must be off, flow, confidence, or fallback.");
            }
            options.debug_view = *parsed;
        } else if (argument == L"--upscale") {
            const std::wstring value = RequireValue(argc, argv, index, L"--upscale");
            const auto parsed = osss::ParseUpscaleMode(value);
            if (!parsed) {
                throw std::invalid_argument("--upscale must be off, auto, or always.");
            }
            options.upscale_mode = *parsed;
        } else if (argument == L"--sharpness") {
            const std::wstring value = RequireValue(argc, argv, index, L"--sharpness");
            options.upscale_sharpness = std::stof(value);
            if (!osss::IsValidSharpness(options.upscale_sharpness)) {
                throw std::invalid_argument("--sharpness must be from 0 through 1.");
            }
        } else if (argument == L"--output-mode") {
            const std::wstring value = RequireValue(argc, argv, index, L"--output-mode");
            const auto parsed = osss::ParseOutputMode(value);
            if (!parsed) {
                throw std::invalid_argument("--output-mode must be overlay or fullscreen.");
            }
            options.output_mode = *parsed;
        } else if (argument == L"--performance-mode") {
            const std::wstring value = RequireValue(argc, argv, index, L"--performance-mode");
            if (value == L"on") {
                options.performance_mode = true;
            } else if (value == L"off") {
                options.performance_mode = false;
            } else {
                throw std::invalid_argument("--performance-mode must be on or off.");
            }
        } else if (argument == L"--temporal-prior") {
            const std::wstring value = RequireValue(argc, argv, index, L"--temporal-prior");
            if (value == L"on") {
                options.temporal_prior = true;
            } else if (value == L"off") {
                options.temporal_prior = false;
            } else {
                throw std::invalid_argument("--temporal-prior must be on or off.");
            }
        } else if (argument == L"--flow-scale") {
            const std::wstring value = RequireValue(argc, argv, index, L"--flow-scale");
            const auto parsed = osss::ParseFlowScale(value);
            if (!parsed) {
                throw std::invalid_argument(
                    "--flow-scale must be auto, quality, performance, or ultra-performance.");
            }
            options.flow_scale = *parsed;
        } else if (argument == L"--stop-event") {
            options.stop_event = RequireValue(argc, argv, index, L"--stop-event");
            if (options.stop_event->empty()) {
                throw std::invalid_argument("--stop-event must not be empty.");
            }
        } else if (argument == L"--stats-overlay") {
            const std::wstring value = RequireValue(argc, argv, index, L"--stats-overlay");
            if (value == L"on") {
                options.stats_overlay = true;
            } else if (value == L"off") {
                options.stats_overlay = false;
            } else {
                throw std::invalid_argument("--stats-overlay must be on or off.");
            }
        } else if (argument == L"--target-fps") {
            const std::wstring value = RequireValue(argc, argv, index, L"--target-fps");
            if (value == L"auto") {
                options.target_fps.reset();
            } else {
                options.target_fps = ParseTargetFps(value);
            }
        } else if (
            argument == L"--max-multiplier" ||
            argument == L"--multiplier" ||
            argument == L"-m") {
            options.max_multiplier = ParseMaxMultiplier(
                RequireValue(argc, argv, index, L"--max-multiplier"));
        } else if (argument == L"--buffer") {
            options.buffer_floor = ParseBufferMilliseconds(
                RequireValue(argc, argv, index, L"--buffer"));
        } else if (argument == L"--ceiling-pacing") {
            options.ceiling_pacing = ParseCeilingPacing(
                RequireValue(argc, argv, index, L"--ceiling-pacing"));
        } else if (argument == L"--present-mode") {
            options.present_mode = RequirePresentMode(
                RequireValue(argc, argv, index, L"--present-mode"));
        } else if (argument == L"--pacing") {
            options.pacing_mode = RequirePacingMode(
                RequireValue(argc, argv, index, L"--pacing"));
        } else if (argument == L"--title") {
            options.title = RequireValue(argc, argv, index, L"--title");
        } else if (argument == L"--hwnd") {
            const std::wstring value = RequireValue(argc, argv, index, L"--hwnd");
            const auto raw_handle = std::stoull(value, nullptr, 0);
            options.window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(raw_handle));
        } else {
            throw std::invalid_argument(
                winrt::to_string(L"Unknown argument: " + std::wstring(argument)));
        }
    }

    if (!osss::IsValidMultiplier(options.max_multiplier)) {
        throw std::invalid_argument("--max-multiplier must be an integer from " +
            std::to_string(osss::kMinimumMultiplier) + " through " +
            std::to_string(osss::kMaximumMultiplier) + ".");
    }
    if (options.window && options.title) {
        throw std::invalid_argument("Choose either --hwnd or --title, not both.");
    }
    return options;
}

void PrintWindows() {
    const DWORD own_process = GetCurrentProcessId();
    for (const auto& entry : osss::ListCapturableWindows()) {
        if (entry.process_id == own_process) {
            continue;
        }
        std::wcout
            << L"0x" << std::hex << entry.handle.Native()
            << std::dec << L"  pid=" << entry.process_id << L"  "
            << (entry.process_name.empty() ? L"(unknown)" : osss::ToWide(entry.process_name))
            << L"  " << osss::ToWide(entry.title) << L'\n';
    }
}

// Executable file name for a resolved target, via the catalog that already
// collects it. Empty when the process cannot be opened, which is the same
// condition WindowEntry documents -- and an empty name never matches a profile
// section, so an unreadable process quietly means "no profile" rather than an
// error.
std::wstring TargetExecutableName(const HWND target) {
    const auto handle = osss::WindowHandle::FromNative(target);
    for (const osss::WindowEntry& entry : osss::ListCapturableWindows()) {
        if (entry.handle == handle) {
            return osss::ToWide(entry.process_name);
        }
    }
    return {};
}

HWND ResolveTarget(const Options& options) {
    if (options.window) {
        if (!IsWindow(*options.window)) {
            throw std::runtime_error("--hwnd does not identify a current top-level window.");
        }
        return *options.window;
    }

    if (!options.title || options.title->empty()) {
        throw std::invalid_argument("Select a target with --title or --hwnd. Use --list-windows to inspect candidates.");
    }

    auto matches = osss::FindWindowsMatching(osss::ToUtf8(*options.title));
    const DWORD own_process = GetCurrentProcessId();
    std::erase_if(matches, [own_process](const osss::WindowEntry& entry) {
        return entry.process_id == own_process;
    });
    if (matches.empty()) {
        throw std::runtime_error(
            "No visible, restored window matched --title by title or executable name.");
    }
    if (matches.size() > 1) {
        std::wcerr << L"--title matched more than one window:\n";
        for (const auto& match : matches) {
            std::wcerr
                << L"  0x" << std::hex << match.handle.Native()
                << std::dec << L"  "
                << (match.process_name.empty() ? L"(unknown)" : osss::ToWide(match.process_name))
                << L"  " << osss::ToWide(match.title) << L'\n';
        }
        throw std::runtime_error("Use a more specific --title or pass --hwnd.");
    }
    return reinterpret_cast<HWND>(matches.front().handle.Native());
}

ResolvedTarget ResolveTargetRate(const Options& options, const HWND target) {
    if (options.target_fps) {
        return ResolvedTarget{
            osss::FrameRate::FromFps(*options.target_fps),
            std::nullopt,
            true,
        };
    }
    const auto display_refresh = osss::WindowDisplayRefreshInfo(osss::WindowHandle::FromNative(target));
    if (!display_refresh || !display_refresh->active_rate.IsValid()) {
        throw std::runtime_error(
            "OSSS could not read the target display refresh rate. Pass --target-fps explicitly.");
    }
    return ResolvedTarget{
        display_refresh->active_rate,
        display_refresh,
        false,
    };
}

// Compiles the motion shaders into the per-user bytecode cache and exits.
//
// Runtime HLSL compilation is unavoidable -- the shaders are inline source and
// specialise against nothing -- but waiting for it is not. The first compile
// costs several seconds, dominated by one entry point; every later start reads
// the cache in a fraction of a second. The launcher runs this in the background
// when it opens, so by the time a target is picked the cache is already warm.
//
// Deliberately creates no window and registers no hotkey: it must be safe to run
// unattended while the user is doing something else.
int RunWarmShaderCache() {
    osss::Renderer renderer;
    renderer.InitializeDevice();
    const std::string error = renderer.InterpolatorError();
    if (!error.empty()) {
        std::cerr << "Shader cache warm-up failed: " << error << '\n';
        return 1;
    }
    std::wcout << L"Motion shaders are cached in " << osss::ShaderCacheDirectory() << L'\n';
    return 0;
}

int RunSelfTest(
    const osss::FlowScale flow_scale,
    const bool performance_mode,
    const osss::OutputMode output_mode,
    const osss::PacingMode pacing_mode) {
    if (!osss::IsPerMonitorV2DpiAware()) {
        throw std::runtime_error("Per-monitor V2 DPI awareness self-test failed.");
    }

    osss::Renderer renderer;
    renderer.InitializeDevice();
    renderer.SetFlowScale(flow_scale);
    renderer.SetPerformanceMode(performance_mode);
    renderer.SetOutputMode(output_mode);
    renderer.SetPacingMode(pacing_mode);
    renderer.SetUpscaleMode(osss::UpscaleMode::always);
    const std::string interpolator_error = renderer.InterpolatorError();
    if (!interpolator_error.empty()) {
        throw std::runtime_error("Motion interpolator self-test failed: " + interpolator_error);
    }

    osss::StatsOverlay stats_overlay;
    constexpr osss::IntRect overlay_test_bounds{0, 0, 1280, 720};
    renderer.CreateOutputWindow(ToRect(overlay_test_bounds), 6, 240.0);
    if (!renderer.FrameLatencyWaitableObject()) {
        throw std::runtime_error("Waitable swap-chain self-test failed.");
    }
    // The pacing mode's one hard requirement of the swap chain. Paced and
    // unpaced must keep latency one or a present can queue hidden delay;
    // queued must get two or the render-ahead blocks every other slot and the
    // mode silently degrades to paced.
    if (renderer.MaximumFrameLatency() != osss::PacingModeMaximumFrameLatency(pacing_mode)) {
        throw std::runtime_error(
            "Swap-chain maximum frame latency does not match the pacing mode (" +
            std::string(osss::PacingModeName(pacing_mode)) + ").");
    }
    if (!renderer.WaitForPresentationSlot(1000)) {
        throw std::runtime_error("First-frame wait-before-render self-test failed.");
    }
    renderer.Render(1.0F);
    SendMessageW(
        renderer.OutputWindow(),
        WM_SIZE,
        SIZE_RESTORED,
        MAKELPARAM(1279, 719));
    if (!renderer.FrameLatencyWaitableObject()) {
        throw std::runtime_error("Waitable swap-chain resize-flag self-test failed.");
    }
    const LONG_PTR output_style = GetWindowLongPtrW(renderer.OutputWindow(), GWL_EXSTYLE);
    if (output_mode == osss::OutputMode::overlay) {
        constexpr LONG_PTR required_output_style =
            WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
            WS_EX_TOPMOST;
        if ((output_style & required_output_style) != required_output_style ||
            SendMessageW(renderer.OutputWindow(), WM_NCHITTEST, 0, 0) != HTTRANSPARENT) {
            throw std::runtime_error(
                "Generated-frame surface did not preserve click-through behavior.");
        }
    } else {
        // The single property fullscreen mode exists to obtain. A layered
        // window is composed through a redirection surface whatever the swap
        // chain asks for, so if this bit is ever set here the mode is silently
        // doing nothing and no variable-refresh display will follow the output
        // clock. Assert it rather than trusting the construction path.
        if ((output_style & WS_EX_LAYERED) != 0) {
            throw std::runtime_error(
                "Fullscreen output kept WS_EX_LAYERED and can never reach independent flip.");
        }
        constexpr LONG_PTR required_fullscreen_style = WS_EX_NOACTIVATE | WS_EX_TOPMOST;
        if ((output_style & required_fullscreen_style) != required_fullscreen_style) {
            throw std::runtime_error(
                "Fullscreen output must stay topmost and must never take focus.");
        }
    }
    // The present mode must resolve to something coherent before anything is
    // presented, because the swap-chain flags are chosen from it and a
    // mismatched pair only fails later, at the first resize.
    const auto present_status = renderer.PresentStatus();
    if (present_status.requested != osss::PresentMode::automatic) {
        throw std::runtime_error("Present mode self-test started from the wrong request.");
    }
    if (present_status.effective != osss::PresentMode::vsync &&
        present_status.effective != osss::PresentMode::tearing) {
        throw std::runtime_error("Present mode did not resolve to a concrete mode.");
    }
    if (present_status.effective == osss::PresentMode::tearing &&
        !present_status.tearing_supported) {
        throw std::runtime_error("Tearing was selected without DXGI reporting support for it.");
    }

    // The overlay is topmost, opaque and click-through: if Show/Hide does not
    // round-trip, a backgrounded target leaves a frozen frame covering the
    // display and nothing underneath it can be seen or reached.
    renderer.Show();
    if (!renderer.OutputVisible() || !IsWindowVisible(renderer.OutputWindow())) {
        throw std::runtime_error("Generated-frame surface did not become visible.");
    }
    renderer.Hide();
    if (renderer.OutputVisible() || IsWindowVisible(renderer.OutputWindow())) {
        throw std::runtime_error("Generated-frame surface could not be hidden.");
    }
    renderer.Show();
    if (!renderer.OutputVisible() || !IsWindowVisible(renderer.OutputWindow())) {
        throw std::runtime_error("Generated-frame surface did not survive a hide/show cycle.");
    }
    renderer.Hide();

    if (!stats_overlay.Create(overlay_test_bounds, 6, 240.0, true)) {
        throw std::runtime_error("Statistics overlay self-test failed.");
    }
    stats_overlay.Update(60.0, 360.0);
    RECT overlay_window_bounds{};
    const HWND overlay_window = reinterpret_cast<HWND>(stats_overlay.Window().Native());
    const UINT overlay_monitor_dpi = GetDpiForWindow(overlay_window);
    constexpr UINT minimum_overlay_layout_dpi = 72;
    constexpr int overlay_offset_dip = 14;
    constexpr int overlay_width_dip = 500;
    constexpr int overlay_height_dip = 216;
    const int overlay_target_width = overlay_test_bounds.width;
    const int overlay_target_height = overlay_test_bounds.height;
    const UINT width_fit_dpi = static_cast<UINT>(
        static_cast<std::int64_t>(overlay_target_width) * 96 /
        (overlay_width_dip + 2 * overlay_offset_dip));
    const UINT height_fit_dpi = static_cast<UINT>(
        static_cast<std::int64_t>(overlay_target_height) * 96 /
        (overlay_height_dip + 2 * overlay_offset_dip));
    const UINT overlay_layout_dpi = std::max(
        minimum_overlay_layout_dpi,
        std::min(overlay_monitor_dpi, std::min(width_fit_dpi, height_fit_dpi)));
    const int expected_overlay_width = MulDiv(overlay_width_dip, overlay_layout_dpi, 96);
    const int expected_overlay_height = MulDiv(overlay_height_dip, overlay_layout_dpi, 96);
    if (!GetWindowRect(overlay_window, &overlay_window_bounds) ||
        overlay_window_bounds.right - overlay_window_bounds.left != expected_overlay_width ||
        overlay_window_bounds.bottom - overlay_window_bounds.top != expected_overlay_height) {
        throw std::runtime_error("Statistics overlay did not scale its layout for the monitor DPI.");
    }
    const LONG_PTR overlay_style = GetWindowLongPtrW(overlay_window, GWL_EXSTYLE);
    constexpr LONG_PTR required_overlay_style =
        WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW;
    if ((overlay_style & required_overlay_style) != required_overlay_style ||
        SendMessageW(overlay_window, WM_NCHITTEST, 0, 0) != HTTRANSPARENT) {
        throw std::runtime_error("Statistics overlay did not preserve click-through behavior.");
    }

    for (int multiplier = osss::kMinimumMultiplier; multiplier <= osss::kMaximumMultiplier;
         ++multiplier) {
        if (!osss::IsValidMultiplier(multiplier)) {
            throw std::runtime_error("Accepted multiplier range self-test failed.");
        }
    }
    if (osss::IsValidMultiplier(osss::kMinimumMultiplier - 1) ||
        osss::IsValidMultiplier(osss::kMaximumMultiplier + 1) ||
        !osss::IsValidTargetFps(240.0) || osss::IsValidTargetFps(0.0)) {
        throw std::runtime_error("Target-rate limit self-test failed.");
    }
    const auto start = osss::OutputClock::Clock::now();
    osss::OutputClock output_clock(osss::FrameRate{144, 1});
    output_clock.Start(start);
    const auto first_deadline = output_clock.TakeLatestDue(start);
    const auto delayed_deadline = output_clock.TakeLatestDue(start + std::chrono::milliseconds(20));
    if (!first_deadline || !delayed_deadline || delayed_deadline->slot != 2 ||
        output_clock.MissedDeadlineCount() != 1) {
        throw std::runtime_error("Target-owned output clock self-test failed.");
    }

    std::wcout << L"Direct3D 11 adapter: " << renderer.AdapterName() << L'\n';
    std::wcout
        << L"Interpolator: " << winrt::to_hstring(renderer.InterpolatorDescription()).c_str() << L'\n'
        << L"Stop hotkey:  " << (renderer.StopHotkeyDescription().empty()
            ? std::wstring(L"NONE -- every candidate chord is already owned by another process")
            : renderer.StopHotkeyDescription()) << L'\n'
        << L"Gen toggle:   " << (renderer.ToggleHotkeyDescription().empty()
            ? std::wstring(L"NONE -- every candidate chord is already owned by another process")
            : renderer.ToggleHotkeyDescription()) << L'\n'
        << L"Pacing:       " << osss::PacingModeArgument(pacing_mode)
        << L" (maximum frame latency " << renderer.MaximumFrameLatency() << L")\n"
        << L"Motion shaders, click-through surfaces, FPS overlay, and target-driven pacing "
           L"self-test passed.\n";
    if (renderer.StopHotkeyDescription().empty()) {
        // Not a hard failure: the machine, not the code, is out of chords. But
        // never let the banner promise a shortcut that does not exist.
        std::cerr
            << "Warning: this machine has no free stop hotkey. Sessions must be ended "
               "from the launcher or Task Manager.\n";
    }
    return 0;
}

// The output window is topmost, opaque and click-through. Leaving it up while
// the target is minimized or in the background paints a frozen frame over the
// whole display and passes every click to whatever is underneath -- which is not
// the game, so the machine reads as hung. Presenting is gated on the target
// still being the window the user is looking at.
// What the banner says about pacing. A user who asked for tearing and silently
// got vsync would otherwise have no way to learn why their frame times still
// alternate, so the requested mode is named whenever it was not honoured.
[[nodiscard]] std::wstring PresentModeBanner(const osss::PresentModeStatus& status) {
    std::wstring text;
    switch (status.effective) {
    case osss::PresentMode::tearing:
        text = L"tearing (sync interval 0, ALLOW_TEARING) -- the output clock sets "
               L"handover time; DWM still composes this overlay, so no literal tearing";
        break;
    case osss::PresentMode::vsync:
    case osss::PresentMode::automatic:
        text = L"vsync (sync interval 1) -- presents are held to a vblank";
        break;
    }
    if (status.requested == osss::PresentMode::tearing &&
        status.effective != osss::PresentMode::tearing) {
        text += L"; --present-mode tearing was requested but DXGI reports no "
                L"ALLOW_TEARING support on this adapter";
    }
    return text;
}

// What the banner says about the pacing mode: the mechanism, not the name, so
// a reader can tell from the banner alone how many frames may be in flight and
// whether the deadline grid is in charge.
[[nodiscard]] std::wstring PacingModeBanner(
    const osss::PacingMode mode,
    const unsigned maximum_frame_latency) {
    std::wstring text = osss::PacingModeArgument(mode);
    switch (mode) {
    case osss::PacingMode::unpaced:
        text += L" -- no deadline grid; each frame is selected for now and presented as soon "
                L"as a back buffer is free (max frame latency ";
        break;
    case osss::PacingMode::queued:
        text += L" -- slot k+1 is rendered while slot k waits for its deadline; presents "
                L"still hand over on the deadline grid (max frame latency ";
        break;
    case osss::PacingMode::paced:
        text += L" -- each slot is selected, rendered, and presented at its deadline "
                L"(max frame latency ";
        break;
    }
    text += std::to_wstring(maximum_frame_latency) + L")";
    return text;
}

[[nodiscard]] bool TargetIsPresentable(const HWND target) noexcept {
    if (!IsWindow(target) || IsIconic(target) || !IsWindowVisible(target)) {
        return false;
    }
    const HWND foreground = GetForegroundWindow();
    if (!foreground) {
        return false;
    }
    if (foreground == target) {
        return true;
    }
    // Games routinely hand the foreground to a sibling window of the same
    // process -- a splash, an IME candidate list, or a second swap-chain window.
    // Treat those as the target still being in front.
    DWORD target_process = 0;
    DWORD foreground_process = 0;
    static_cast<void>(GetWindowThreadProcessId(target, &target_process));
    static_cast<void>(GetWindowThreadProcessId(foreground, &foreground_process));
    return target_process != 0 && target_process == foreground_process;
}

// Telemetry counters are unsigned and are not all monotonic: restarting the
// capture session rewinds CaptureSession's own counts to zero while the
// sampling baseline still holds the pre-restart value. A plain subtraction then
// underflows to ~1.8e19 and prints as the raw capture rate. Saturate instead.
[[nodiscard]] constexpr std::uint64_t SampleDelta(
    const std::uint64_t current,
    const std::uint64_t previous) noexcept {
    return current >= previous ? current - previous : current;
}

// True when some process holds the display in D3D exclusive fullscreen. WGC
// cannot capture that path (README "Limitations"), and the symptom is a frozen
// overlay rather than an error, so it is worth naming up front.
[[nodiscard]] bool ExclusiveFullscreenActive() noexcept {
    QUERY_USER_NOTIFICATION_STATE state{};
    if (FAILED(SHQueryUserNotificationState(&state))) {
        return false;
    }
    return state == QUNS_RUNNING_D3D_FULL_SCREEN;
}

int RunFrameGeneration(const Options& options) {
    using Clock = std::chrono::steady_clock;

    ScopedPresentationPriority presentation_priority;

    const HWND target = ResolveTarget(options);
    const ResolvedTarget resolved_target = ResolveTargetRate(options, target);
    const double target_fps = resolved_target.rate.AsDouble();
    ScopedHandle stop_event;
    if (options.stop_event) {
        stop_event.Reset(OpenEventW(SYNCHRONIZE, FALSE, options.stop_event->c_str()));
        if (!stop_event) {
            winrt::throw_last_error();
        }
    }
    const auto initial_bounds = osss::ExtendedWindowBounds(osss::WindowHandle::FromNative(target));
    if (!initial_bounds) {
        throw std::runtime_error("The target is minimized or has no usable window bounds.");
    }

    osss::Renderer renderer;
    renderer.InitializeDevice();
    // Before CreateOutputWindow: the swap-chain flags depend on the mode, and
    // the swap chain is created there.
    // Before CreateOutputWindow: it decides the window styles.
    renderer.SetOutputMode(options.output_mode);
    renderer.SetDebugView(options.debug_view);
    renderer.SetUpscaleMode(options.upscale_mode);
    renderer.SetUpscaleSharpness(options.upscale_sharpness);
    renderer.SetPresentMode(options.present_mode);
    // Before CreateOutputWindow: it fixes the swap chain's maximum frame
    // latency and buffer count.
    renderer.SetPacingMode(options.pacing_mode);
    renderer.SetMotionEnabled(options.motion_interpolation);
    if (!options.ui_mask.empty() && !renderer.SetUiMask(options.ui_mask)) {
        std::cerr
            << "UI mask could not be applied; continuing without it: "
            << renderer.InterpolatorError() << '\n';
    }
    renderer.SetAutoUiMaskEnabled(options.ui_mask_auto);
    renderer.SetFlowScale(options.flow_scale);
    if (!renderer.SetPerformanceMode(options.performance_mode)) {
        std::cerr
            << "Performance mode could not be applied; continuing without it: "
            << renderer.InterpolatorError() << '\n';
    }
    renderer.SetTemporalPriorEnabled(options.temporal_prior);
    osss::CaptureSession capture(renderer.Device());
    capture.Start(target, target_fps);
    renderer.CreateOutputWindow(ToRect(*initial_bounds), options.max_multiplier, target_fps);
    osss::StatsOverlay stats_overlay;
    const bool stats_overlay_wanted =
        options.stats_overlay && options.output_mode != osss::OutputMode::fullscreen;
    if (options.stats_overlay && !stats_overlay_wanted) {
        std::cerr
            << "FPS overlay disabled: a second topmost window would demote fullscreen "
               "output out of independent flip.\n";
    }
    if (stats_overlay_wanted &&
        !stats_overlay.Create(
            *initial_bounds,
            options.max_multiplier,
            target_fps,
            options.motion_interpolation)) {
        std::cerr << "FPS overlay could not be created; continuing without it.\n";
    }

    osss::OutputClock output_clock(resolved_target.rate);
    osss::SourceTimeline source_timeline;
    osss::FrameSelector frame_selector(
        options.max_multiplier,
        options.buffer_floor,
        options.ceiling_pacing);
    // One target slot, as a duration. The queued mode selects slot k+1 while
    // slot k is being presented, and pays for that on the media clock rather
    // than by presenting early; see FrameSelector::SetLookahead.
    const auto target_slot_period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / target_fps));
    const auto lookahead =
        target_slot_period * osss::PacingModeLookaheadSlots(options.pacing_mode);
    frame_selector.SetLookahead(lookahead);
    const bool clock_owned = osss::PacingModeUsesOutputClock(options.pacing_mode);
    const bool render_ahead = osss::PacingModeRendersAhead(options.pacing_mode);
    DeadlineWaiter deadline_waiter;
    osss::PacingMonitor pacing_monitor;
    std::uint64_t consumed_raw_sequence = 0;
    std::uint64_t interpolated_presentations = 0;
    auto sample_started = Clock::now();
    auto last_follow = sample_started;
    output_clock.Start(sample_started);
    std::uint64_t sampled_raw_frames = 0;
    std::uint64_t sampled_unique_frames = 0;
    std::uint64_t sampled_submitted_frames = 0;
    std::uint64_t sampled_confirmed_frames = 0;
    std::uint64_t sampled_interpolated_presentations = 0;
    osss::FrameSelectionCounters selection_counts{};
    osss::FrameSelectionCounters sampled_selection_counts{};
    std::uint64_t observed_discontinuities = 0;
    // Last toggle-hotkey count the loop acted on; see the poll below.
    std::uint64_t observed_generation_toggles = 0;
    osss::FrameSelection latest_selection{};
    double latest_capture_to_present_milliseconds = 0.0;
    std::string reported_motion_error;
    // Queued pacing: the slot selected and drawn ahead of its deadline, if any.
    // `prepared_rendered` is false when the slot was selected but could not be
    // drawn (frame missing, or invalidated by a discontinuity), in which case
    // it is counted missed at its deadline rather than presented stale.
    std::optional<std::uint64_t> prepared_slot;
    osss::FrameSelection prepared_selection{};
    bool prepared_rendered = false;
    // How many presents were of a frame drawn ahead of its deadline, versus
    // served at the deadline because nothing was prepared. Reported so the
    // queued mode's claim can be checked from the console rather than assumed:
    // if `served-late` is not near zero the render-ahead is not happening.
    std::uint64_t presents_rendered_ahead = 0;
    std::uint64_t presents_served_at_deadline = 0;
    std::uint64_t sampled_presents_rendered_ahead = 0;
    std::uint64_t sampled_presents_served_at_deadline = 0;
    // Unpaced pacing: what the last present showed, so an unchanged real frame
    // is not re-presented at composition rate while nothing new has arrived.
    bool unpaced_last_present_was_real = false;
    std::uint64_t unpaced_last_real_sequence = 0;

    std::wcout
        << L"Capturing: " << osss::ToWide(osss::WindowTitle(osss::WindowHandle::FromNative(target))) << L'\n'
        << L"Adapter:   " << renderer.AdapterName() << L'\n'
        << L"Target:    " << std::fixed << std::setprecision(6) << target_fps << L" FPS  ("
        << resolved_target.rate.numerator << L"/" << resolved_target.rate.denominator << L")"
        << (resolved_target.manual ? L" (manual)\n" : L" (active display path)\n")
        << L"Limit:     up to " << options.max_multiplier << L"x interpolation\n"
        << L"Capture:   ";
    if (const auto requested_capture_fps = capture.RequestedUpdateFps()) {
        std::wcout
            << L"up to " << std::fixed << std::setprecision(1) << *requested_capture_fps
            << L" FPS requested (headroom over the target so real frames are not throttled)\n";
    } else {
        std::wcout << L"system cadence (high-rate request unavailable)\n";
    }
    std::wcout
        << L"Motion:    " << (options.motion_interpolation
            ? winrt::to_hstring(renderer.InterpolatorDescription()).c_str()
            : L"disabled (temporal blend A/B mode)") << L'\n'
        << L"Debug:     " << osss::DebugViewArgument(options.debug_view)
        << (options.debug_view == osss::DebugView::off
            ? L""
            : L" (diagnostic view; this is not normal output)") << L'\n'
        << L"Output:    " << osss::OutputModeArgument(options.output_mode)
        << (renderer.PresentStatus().independent_flip_eligible
            ? L" (independent-flip eligible; confirm with PresentMon)"
            : L" (DWM-composed)") << L'\n'
        << L"FPS HUD:   " << (stats_overlay.Window() ? L"on" : L"off") << L'\n'
        << L"UI mask:   " << (options.ui_mask.empty()
            ? std::wstring(L"none")
            : std::to_wstring(options.ui_mask.size()) + L" region(s): " +
                osss::FormatUiMaskRects(options.ui_mask) +
                (options.motion_interpolation ? L"" : L" (ignored by blend mode)")) << L'\n'
        << L"Auto mask: " << (options.ui_mask_auto
            ? L"on (static overlays detected from motion)"
            : L"off") << L'\n'
        << L"Queue:     target 1 (one source period + "
        << options.buffer_floor.count()
        << L" ms adaptive jitter floor; grows on underrun"
        << (lookahead > Clock::duration::zero()
            ? L" + one target slot of render-ahead lookahead)\n"
            : L")\n")
        << L"Present:   " << PresentModeBanner(renderer.PresentStatus()) << L'\n'
        << L"Pacing:    " << PacingModeBanner(options.pacing_mode, renderer.MaximumFrameLatency())
        << L'\n'
        << L"Clock:     " << (clock_owned
            ? L"fixed rational target deadlines; stale slots are skipped"
            : L"none -- the loop presents whenever a back buffer is free") << L'\n'
        << L"Priority:  " << (presentation_priority.UsingMmcss()
            ? L"MMCSS Games"
            : L"THREAD_PRIORITY_HIGHEST fallback") << L'\n'
        << L"Ceiling:   " << (options.ceiling_pacing == osss::FrameSelector::CeilingPacing::even
            ? L"even cadence when bound"
            : L"distributed cadence") << L'\n'
        << L"Overlay:   hidden while the target is minimized, in the background, or\n"
           L"           the source has been silent for over 1 s\n"
        << L"Stop:      " << (renderer.StopHotkeyDescription().empty()
            ? std::wstring(L"unavailable (every candidate chord is already taken)")
            : renderer.StopHotkeyDescription()) << L'\n'
        << L"Gen on/off: " << (renderer.ToggleHotkeyDescription().empty()
            ? std::wstring(L"unavailable (every candidate chord is already taken)")
            : renderer.ToggleHotkeyDescription() +
                L" -- pauses generation without ending the session") << L'\n';
    if (renderer.StopHotkeyDescription().empty()) {
        std::cerr
            << "No stop hotkey could be registered: another process already owns every "
               "candidate chord. Stop this session from the launcher, or end osss.exe "
               "from Task Manager.\n";
    }
    if (ExclusiveFullscreenActive()) {
        std::cerr
            << "A Direct3D exclusive-fullscreen application is holding the display. "
               "Windows Graphics Capture cannot read that path, so the overlay would "
               "show a frozen frame. Switch the target to borderless or windowed.\n";
    }
    if (resolved_target.display) {
        std::wcout
            << L"Display:   "
            << (resolved_target.display->rational_path_available
                ? L"rational CCD path"
                : L"integer compatibility fallback")
            << (resolved_target.display->dynamic_refresh_enabled
                ? L"; Windows DRR boost enabled"
                : L"; fixed/undetected DRR")
            << L'\n';
    }

    if (options.motion_interpolation) {
        reported_motion_error = renderer.InterpolatorError();
        if (!reported_motion_error.empty()) {
            std::cerr
                << "Motion interpolation is unavailable; continuing with temporal blend: "
                << reported_motion_error << '\n';
        }
    }

    // The display's refresh period, used only to decide whether the target grid
    // and the vblank grid are commensurate and, if so, to phase-align them.
    // Nothing here changes the target rate; a refresh rate that cannot be read
    // simply leaves the clock unaligned, which is what it always was.
    const auto display_refresh_period = [&]() -> Clock::duration {
        if (!resolved_target.display || !resolved_target.display->active_rate.IsValid()) {
            return Clock::duration::zero();
        }
        const double refresh_fps = resolved_target.display->active_rate.AsDouble();
        if (!(refresh_fps > 0.0)) {
            return Clock::duration::zero();
        }
        return std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / refresh_fps));
    }();
    // How far ahead of a vblank a deadline should land: enough time to select a
    // pair, run fusion, and present into that same vblank. Sized from the GPU
    // work this loop actually does rather than from a fraction of the refresh
    // period, so it does not shrink to nothing on a fast panel.
    constexpr auto kVblankLead = std::chrono::microseconds(1500);

    // How long the source may go silent before the overlay stops presenting.
    // `FrameSelector` already holds the newest real frame through a stall, which
    // is right for a hiccup, but it holds forever: a capture that dies leaves a
    // frozen frame painted over the target at the full target rate, with
    // capture-to-present climbing without bound. Windows Graphics Capture only
    // delivers on content change, so a genuinely static source is silent too --
    // hiding then is visually a no-op, because what shows through is the same
    // unchanged window.
    constexpr auto kSourceSilenceLimit = std::chrono::milliseconds(1000);
    auto last_unique_arrival = Clock::now();

    bool running = true;
    bool target_presentable = true;
    bool target_was_iconic = IsIconic(target) != FALSE;
    while (running && IsWindow(target)) {
        if (stop_event && WaitForSingleObject(stop_event.Get(), 0) == WAIT_OBJECT_0) {
            break;
        }
        running = renderer.PumpMessages();
        if (!running) {
            break;
        }

        // Minimizing and restoring the target permanently kills the capture:
        // Windows Graphics Capture stops delivering and never recovers on its
        // own, because the frame pool was sized against a surface that no longer
        // exists. Measured against the test animation -- raw and unique both go
        // to zero and stay there. A game that minimizes on focus loss hits this
        // every alt-tab, so rebuild the session on the restore edge.
        const bool target_iconic_now = IsIconic(target) != FALSE;
        if (target_was_iconic && !target_iconic_now) {
            try {
                capture.Stop();
                capture.Start(target, target_fps);
                // Start() restarts raw sequence numbering at one, so the drain
                // cursor has to go back with it. Unique sequences come from
                // SourceTimeline's own monotonic counter, which ResetHistory
                // deliberately does not rewind -- that is what keeps the
                // renderer's frame history from matching a stale entry.
                consumed_raw_sequence = 0;
                source_timeline.ResetHistory();
                observed_discontinuities = source_timeline.DiscontinuityCount();
                frame_selector.Reset();
                // A slot drawn ahead from the old session's frames is no
                // longer worth presenting; let its deadline count it missed.
                prepared_rendered = false;
                last_unique_arrival = Clock::now();
                // CaptureSession::Start zeroes its own raw/duplicate/drop
                // counts. The sampling baseline has to follow, or the next
                // interval subtracts a larger previous from a smaller current.
                sampled_raw_frames = 0;
            } catch (const std::exception& error) {
                std::cerr
                    << "Capture could not be restarted after the target was restored: "
                    << error.what() << '\n';
            }
        }
        target_was_iconic = target_iconic_now;

        const bool source_silent =
            Clock::now() - last_unique_arrival > kSourceSilenceLimit;
        const bool presentable_now = TargetIsPresentable(target) && !source_silent;
        if (presentable_now != target_presentable) {
            target_presentable = presentable_now;
            if (target_presentable) {
                // Whatever pair was on screen is stale by however long we spent
                // hidden; make the selector re-acquire instead of interpolating
                // across the gap.
                frame_selector.Reset();
                prepared_slot.reset();
                prepared_rendered = false;
                unpaced_last_present_was_real = false;
                if (renderer.HasCapturedFrame()) {
                    renderer.Show();
                    stats_overlay.Show();
                }
            } else {
                renderer.Hide();
                stats_overlay.Hide();
            }
            // The interval spanning a hide is the length of the hide, not a
            // pacing failure. Charging it would put a multi-second outlier into
            // p95 and max every time the user alt-tabbed.
            pacing_monitor.Reset();
        }

        capture.DrainClassifiedFramesAfter(
            consumed_raw_sequence,
            [&](ID3D11Texture2D* texture, const osss::CapturedFrameInfo& information) {
                consumed_raw_sequence = std::max(consumed_raw_sequence, information.sequence);
                const auto unique_sequence = source_timeline.Ingest(
                    information.sequence,
                    information.media_time,
                    information.arrival,
                    information.width,
                    information.height,
                    information.duplicate,
                    information.ingest);
                if (source_timeline.DiscontinuityCount() != observed_discontinuities) {
                    observed_discontinuities = source_timeline.DiscontinuityCount();
                    frame_selector.Reset();
                    // A source resize resizes the swap chain and with it the
                    // back buffer a prepared slot was drawn into.
                    prepared_rendered = false;
                }
                if (!unique_sequence) {
                    return;
                }

                last_unique_arrival = Clock::now();
                renderer.StoreCapturedFrame(texture, *unique_sequence);
                if (target_presentable) {
                    renderer.Show();
                    stats_overlay.Show();
                }
            });

        const std::string motion_error = renderer.InterpolatorError();
        if (options.motion_interpolation && !motion_error.empty() &&
            motion_error != reported_motion_error) {
            std::cerr
                << "Motion estimation failed for the selected pair; using temporal blend: "
                << motion_error << '\n';
            reported_motion_error = motion_error;
        }

        const std::string capture_error = capture.Error();
        if (!capture_error.empty()) {
            throw std::runtime_error("Capture failed: " + capture_error);
        }

        // A press is applied here rather than in the window procedure, so the
        // switch lands between output slots instead of inside one. The count is
        // compared rather than consumed: two presses between polls still leave
        // generation where the second one put it.
        if (const std::uint64_t toggles = renderer.GenerationToggleCount();
            toggles != observed_generation_toggles) {
            observed_generation_toggles = toggles;
            const bool enabled = !frame_selector.GenerationEnabled();
            frame_selector.SetGenerationEnabled(enabled);
            // The presents that did not happen while generation was off are a
            // gap, not judder, and charging them to the pacing report would make
            // every A/B look like a pacing failure. This is exactly the case
            // PacingMonitor::Reset documents.
            pacing_monitor.Reset();
            std::wcout << L"Frame generation " << (enabled ? L"ON" : L"OFF") << L'\n';
        }

        // How far the coarse motion search looks, refreshed from the measured
        // source cadence before this iteration's pair is prepared. A fixed
        // radius bounds the search by displacement; scaling it by the period
        // bounds it by velocity instead, which is the quantity that is actually
        // constant about a panning camera. See ResolveCoarseSearchRadius in
        // src/flow_scale.h. Free to call: it stores a double, and the radius
        // only bounds a dynamic loop, so nothing is rebuilt and no prepared pair
        // is dropped.
        renderer.SetSourcePeriod(source_timeline.EstimatedSourcePeriod().count());

        renderer.PollGpuTimings();

        // Binds the selected frames and draws them into the held back buffer.
        // False if a frame is missing from the history or no back buffer is
        // free; it does not present.
        const auto render_selection = [&](const osss::FrameSelection& selection) -> bool {
            const bool selected = selection.mode == osss::FrameSelectionMode::interpolate
                ? renderer.SelectFramePair(
                    selection.previous_unique_sequence,
                    selection.current_unique_sequence)
                : renderer.SelectRealFrame(selection.current_unique_sequence);
            if (!selected || !renderer.WaitForPresentationSlot(0)) {
                return false;
            }
            renderer.Render(selection.alpha);
            return true;
        };
        // Hands the drawn frame over and books the telemetry for it.
        const auto present_selection = [&](const osss::FrameSelection& selection) {
            renderer.Present();
            // Stamped after Present returns, so the interval measured is the one
            // OSSS is answerable for: deadline to handover under a clock,
            // present to present without one. It is not scan-out, and the
            // pacing report says so.
            pacing_monitor.RecordPresent(Clock::now());
            if (selection.mode == osss::FrameSelectionMode::interpolate) {
                ++interpolated_presentations;
            }
            latest_capture_to_present_milliseconds = std::max(
                0.0,
                std::chrono::duration<double, std::milli>(
                    Clock::now() - selection.media_time).count());
        };
        // The paced path for one due slot: select, render, present, all now.
        const auto serve_slot_now = [&](const osss::OutputClock::Deadline& deadline) {
            latest_selection = frame_selector.Select(
                deadline, resolved_target.rate, source_timeline);
            selection_counts.Record(latest_selection.mode);
            if (latest_selection.submit) {
                if (render_selection(latest_selection)) {
                    present_selection(latest_selection);
                    ++presents_served_at_deadline;
                } else {
                    output_clock.RecordMissedSubmission();
                }
            } else if (latest_selection.mode == osss::FrameSelectionMode::no_frame) {
                output_clock.RecordMissedSubmission();
            }
        };

        auto now = Clock::now();
        if (!clock_owned) {
            // Unpaced: no deadline grid. Present whenever a back buffer is free.
            // The slot is taken non-blockingly here or, between iterations, by
            // the waiter below; either way it is held until spent by a Present.
            if (target_presentable) {
                if (!renderer.PresentationSlotAcquired()) {
                    static_cast<void>(renderer.WaitForPresentationSlot(0));
                }
                if (renderer.PresentationSlotAcquired()) {
                    latest_selection = frame_selector.SelectNow(
                        now, resolved_target.rate, source_timeline);
                    // A real frame already on screen is not re-presented: with
                    // generation off, or through a stall, the display keeps the
                    // frame it has until a new source frame arrives, and the
                    // loop sleeps on the capture event instead of spinning on a
                    // slot it holds but has nothing new for.
                    const bool repeat_real =
                        latest_selection.mode != osss::FrameSelectionMode::interpolate &&
                        unpaced_last_present_was_real &&
                        latest_selection.current_unique_sequence ==
                            unpaced_last_real_sequence;
                    if (latest_selection.submit && !repeat_real) {
                        selection_counts.Record(latest_selection.mode);
                        if (render_selection(latest_selection)) {
                            present_selection(latest_selection);
                            unpaced_last_present_was_real =
                                latest_selection.mode != osss::FrameSelectionMode::interpolate;
                            unpaced_last_real_sequence = latest_selection.current_unique_sequence;
                        }
                    }
                }
            }
        } else if (const auto deadline = output_clock.TakeLatestDue(now)) {
            // A slot that comes due while the overlay is hidden is consumed and
            // dropped without recording a missed *submission*. Note this does
            // not zero the missed count while hidden: OutputClock::TakeLatestDue
            // separately counts slots it had to skip, and it owns that decision.
            // Re-anchoring the clock on resume would clear the genuine count
            // too, so `missed` legitimately climbs across a long hide.
            if (!target_presentable) {
                latest_selection = osss::FrameSelection{};
                prepared_slot.reset();
            } else if (prepared_slot) {
                // Queued: this slot (or, if the loop ran late enough that
                // TakeLatestDue skipped past it, an older one) was already
                // selected and drawn. Presenting the older frame anyway is the
                // right call: it is one slot stale, whereas re-rendering now
                // would make this slot late too, and the skip is already in the
                // missed count. Nothing here consults the selector, so each
                // slot is selected exactly once, at prepare time.
                if (prepared_rendered) {
                    present_selection(prepared_selection);
                    ++presents_rendered_ahead;
                } else if (prepared_selection.submit ||
                    prepared_selection.mode == osss::FrameSelectionMode::no_frame) {
                    // Selected but never drawn (frame missing from history), or
                    // nothing to show: the slot goes unfilled.
                    output_clock.RecordMissedSubmission();
                }
                prepared_slot.reset();
            } else {
                // Paced -- or queued with nothing prepared (cold start, or the
                // back buffer was not free in time). Serve the slot now so it is
                // not lost; the next one is prepared ahead below.
                serve_slot_now(*deadline);
            }
        }

        // Queued: prepare the next slot as soon as a back buffer is free. This
        // is the render-ahead. It runs after any due slot has been served, so
        // in the steady state the sequence per slot is: present k at its
        // deadline, immediately select and draw k+1, sleep until k+1's
        // deadline. The selector's lookahead has already moved the media clock
        // back one slot so k+1's source pair is in hand now.
        if (clock_owned && render_ahead && target_presentable && !prepared_slot &&
            renderer.WaitForPresentationSlot(0)) {
            const osss::OutputClock::Deadline next{
                output_clock.NextDeadline(),
                output_clock.NextSlot(),
                0};
            prepared_selection = frame_selector.Select(
                next, resolved_target.rate, source_timeline);
            latest_selection = prepared_selection;
            selection_counts.Record(prepared_selection.mode);
            prepared_rendered = prepared_selection.submit && render_selection(prepared_selection);
            prepared_slot = next.slot;
        }

        // Phase-align the deadline grid to the display, but only under vsync.
        //
        // In tearing mode there is nothing to align to and aligning would be
        // actively wrong: the output clock is what sets presentation time, and
        // a VRR display follows it rather than the other way round.
        //
        // Under vsync the alignment is what turns "the right number of frames"
        // into "evenly spaced frames". Every present is held to a vblank, so a
        // deadline landing just after one waits a whole refresh period; the lead
        // aims each deadline far enough ahead of a vblank to render and present
        // into that same one. The clock rejects the alignment outright when the
        // two grids are incommensurate, which is the case tearing mode exists
        // for.
        if (clock_owned &&
            renderer.PresentStatus().effective == osss::PresentMode::vsync &&
            display_refresh_period > Clock::duration::zero()) {
            if (const auto vblank = renderer.LastVblank()) {
                static_cast<void>(output_clock.PhaseAlignToVblank(
                    *vblank,
                    display_refresh_period,
                    kVblankLead));
            }
        }

        now = Clock::now();
        if (now - last_follow >= std::chrono::milliseconds(250)) {
            renderer.FollowTarget(target);
            stats_overlay.FollowTarget(osss::WindowHandle::FromNative(target));
            last_follow = now;
        }

        if (now - sample_started >= std::chrono::seconds(1)) {
            const double seconds = std::chrono::duration<double>(now - sample_started).count();
            const std::uint64_t raw_frames = capture.CapturedFrameCount();
            const std::uint64_t unique_frames = source_timeline.UniqueFrameCount();
            const auto& presentation = renderer.PresentationStatistics();
            const double raw_capture_fps =
                static_cast<double>(SampleDelta(raw_frames, sampled_raw_frames)) / seconds;
            const double unique_source_fps =
                static_cast<double>(SampleDelta(unique_frames, sampled_unique_frames)) / seconds;
            const double submitted_fps =
                static_cast<double>(
                    SampleDelta(presentation.submitted, sampled_submitted_frames)) / seconds;
            std::optional<double> confirmed_fps;
            if (presentation.statistics_available) {
                confirmed_fps = static_cast<double>(
                    SampleDelta(presentation.confirmed, sampled_confirmed_frames)) / seconds;
            }
            const double realized_multiplier = unique_source_fps > 0.0
                ? submitted_fps / unique_source_fps
                : 0.0;
            // Interval share for the HUD; the console reports the cumulative one
            // below. Both divide by submitted rather than by the target slot count,
            // so a missed slot never inflates the generated fraction. Unique is
            // frames *captured* this interval rather than frames presented, which
            // a queue target of 1 keeps within about one frame over a 1 s window.
            const double generated_share = osss::GeneratedFrameShare(
                SampleDelta(interpolated_presentations, sampled_interpolated_presentations),
                SampleDelta(unique_frames, sampled_unique_frames),
                SampleDelta(presentation.submitted, sampled_submitted_frames));

            osss::RuntimeStats runtime_statistics{};
            runtime_statistics.raw_capture_fps = raw_capture_fps;
            runtime_statistics.unique_source_fps = unique_source_fps;
            runtime_statistics.target_fps = target_fps;
            runtime_statistics.submitted_fps = submitted_fps;
            runtime_statistics.confirmed_fps = confirmed_fps;
            runtime_statistics.required_multiplier = latest_selection.required_multiplier;
            runtime_statistics.allowed_multiplier = latest_selection.allowed_multiplier;
            runtime_statistics.realized_multiplier = realized_multiplier;
            runtime_statistics.generated_share = generated_share;
            runtime_statistics.generation_enabled = frame_selector.GenerationEnabled();
            runtime_statistics.queue_occupancy = latest_selection.queue_occupancy;
            runtime_statistics.queue_delay_milliseconds =
                latest_selection.queue_delay.count() * 1000.0;
            runtime_statistics.capture_to_present_milliseconds =
                latest_capture_to_present_milliseconds;
            runtime_statistics.media_to_ingest_p50_milliseconds =
                source_timeline.MediaToIngestP50().count() * 1000.0;
            runtime_statistics.media_to_ingest_p95_milliseconds =
                source_timeline.MediaToIngestP95().count() * 1000.0;
            // Without a clock there is no target period to be on time against:
            // the intervals are present-to-present and reported as such, and
            // the on-time share and error are not computed at all rather than
            // computed against a rate the loop was never aiming for.
            const auto pacing = pacing_monitor.Summarize(clock_owned ? target_fps : 0.0);
            runtime_statistics.pacing_clock_owned = clock_owned;
            runtime_statistics.pacing_p50_milliseconds = pacing.p50_milliseconds;
            runtime_statistics.pacing_p95_milliseconds = pacing.p95_milliseconds;
            runtime_statistics.pacing_maximum_milliseconds = pacing.maximum_milliseconds;
            runtime_statistics.pacing_mean_absolute_error_milliseconds =
                pacing.mean_absolute_error_milliseconds;
            runtime_statistics.pacing_on_time_fraction = pacing.on_time_fraction;
            runtime_statistics.pacing_sample_count = pacing.sample_count;

            const auto flow_gpu_timing = renderer.FlowGpuTiming();
            const auto fusion_gpu_timing = renderer.FusionGpuTiming();
            runtime_statistics.flow_gpu_p50_milliseconds =
                flow_gpu_timing.p50_milliseconds;
            runtime_statistics.flow_gpu_p95_milliseconds =
                flow_gpu_timing.p95_milliseconds;
            runtime_statistics.fusion_gpu_p50_milliseconds =
                fusion_gpu_timing.p50_milliseconds;
            runtime_statistics.fusion_gpu_p95_milliseconds =
                fusion_gpu_timing.p95_milliseconds;
            runtime_statistics.missed_deadlines = output_clock.MissedDeadlineCount();
            runtime_statistics.duplicate_frames = capture.DuplicateFrameCount();
            runtime_statistics.dropped_frames = capture.DroppedFrameCount();
            runtime_statistics.selection_interval = osss::Difference(
                selection_counts,
                sampled_selection_counts);
            runtime_statistics.selection_cumulative = selection_counts;
            stats_overlay.Update(runtime_statistics);
            std::wcout
                << L"raw=" << std::fixed << std::setprecision(1) << raw_capture_fps
                << L"  unique=" << unique_source_fps
                << L"  target=" << target_fps
                << L"  submitted=" << submitted_fps;
            if (confirmed_fps) {
                std::wcout << L"  display-confirmed=" << *confirmed_fps;
            } else {
                std::wcout << L"  display-confirmed=n/a";
            }
            std::wcout
                << L"  required=" << std::setprecision(2)
                << latest_selection.required_multiplier << L"x"
                << L"  allowed=" << latest_selection.allowed_multiplier << L"x"
                << L"  realized=" << realized_multiplier << L"x"
                << L"  queue=" << latest_selection.queue_occupancy
                << L"/8"
                << L"  queue-delay=" << std::setprecision(1)
                << latest_selection.queue_delay.count() * 1000.0 << L"ms"
                << L"  capture-to-present="
                << latest_capture_to_present_milliseconds << L"ms"
                << L"  media-to-ingest-p50="
                << runtime_statistics.media_to_ingest_p50_milliseconds << L"ms"
                << L"  media-to-ingest-p95="
                << runtime_statistics.media_to_ingest_p95_milliseconds << L"ms"
                << L"  generation="
                << (frame_selector.GenerationEnabled() ? L"on" : L"off")
                << L"  flow-search=" << renderer.CoarseSearchPixels() << L"px"
                << L"  flow-gpu-p50/p95="
                << flow_gpu_timing.p50_milliseconds << L"/"
                << flow_gpu_timing.p95_milliseconds << L"ms"
                << L"  fusion-gpu-p50/p95="
                << fusion_gpu_timing.p50_milliseconds << L"/"
                << fusion_gpu_timing.p95_milliseconds << L"ms"
                << (clock_owned ? L"  pacing-p50/p95/max=" : L"  pacing(free-run)-p50/p95/max=")
                << std::setprecision(2)
                << pacing.p50_milliseconds << L"/"
                << pacing.p95_milliseconds << L"/"
                << pacing.maximum_milliseconds << L"ms";
            if (clock_owned) {
                std::wcout
                    << L"  pacing-on-time=" << std::setprecision(0)
                    << pacing.on_time_fraction * 100.0 << L"%"
                    << L"  pacing-mae=" << std::setprecision(2)
                    << pacing.mean_absolute_error_milliseconds << L"ms"
                    << L"  clock-phase-fixes=" << output_clock.PhaseCorrectionCount()
                    << std::setprecision(1)
                    << L"  missed=" << output_clock.MissedDeadlineCount();
            } else {
                std::wcout << L"  pacing-on-time=n/a  missed=n/a";
            }
            if (render_ahead) {
                std::wcout
                    << L"  render-ahead="
                    << SampleDelta(presents_rendered_ahead, sampled_presents_rendered_ahead)
                    << L"  served-late="
                    << SampleDelta(
                        presents_served_at_deadline, sampled_presents_served_at_deadline);
            }
            std::wcout
                << std::setprecision(1)
                << L"  duplicates=" << capture.DuplicateFrameCount()
                << L"  capture-drops=" << capture.DroppedFrameCount()
                << L"  interpolated=" << interpolated_presentations
                << L"  generated-share=" << std::setprecision(0)
                << osss::GeneratedFrameShare(
                       interpolated_presentations, unique_frames, presentation.submitted) * 100.0
                << L"%"
                 << L"  modes-i="
                 << runtime_statistics.selection_interval.no_frame << L"/"
                 << runtime_statistics.selection_interval.cold_start << L"/"
                 << runtime_statistics.selection_interval.interpolate << L"/"
                 << runtime_statistics.selection_interval.real_frame << L"/"
                 << runtime_statistics.selection_interval.hold << L"/"
                << runtime_statistics.selection_interval.underrun << L"/"
                << runtime_statistics.selection_interval.ceiling_hold << L"/"
                << runtime_statistics.selection_interval.stalled
                 << L"  modes-total="
                 << selection_counts.no_frame << L"/"
                 << selection_counts.cold_start << L"/"
                 << selection_counts.interpolate << L"/"
                << selection_counts.real_frame << L"/"
                << selection_counts.hold << L"/"
                << selection_counts.underrun << L"/"
                << selection_counts.ceiling_hold << L"/"
                << selection_counts.stalled
                << L"\r" << std::flush;
            sample_started = now;
            sampled_raw_frames = raw_frames;
            sampled_unique_frames = unique_frames;
            sampled_submitted_frames = presentation.submitted;
            sampled_confirmed_frames = presentation.confirmed;
            sampled_interpolated_presentations = interpolated_presentations;
            sampled_selection_counts = selection_counts;
            sampled_presents_rendered_ahead = presents_rendered_ahead;
            sampled_presents_served_at_deadline = presents_served_at_deadline;
        }

        // What to sleep on. Under a clock: the next deadline. Without one:
        // a housekeeping tick, so the follow, silence, and telemetry timers
        // above still run when neither a capture nor a slot arrives. The
        // waitable object is added only when the loop wants a back buffer and
        // does not already hold one -- unpaced always, queued when the next
        // slot is not yet prepared -- because waiting on it takes the slot.
        constexpr auto kHousekeepingTick = std::chrono::milliseconds(100);
        const auto wake_at = clock_owned
            ? output_clock.NextDeadline()
            : Clock::now() + kHousekeepingTick;
        const bool wants_slot = target_presentable && !renderer.PresentationSlotAcquired() &&
            (!clock_owned || (render_ahead && !prepared_slot));
        const WaitReason wait_reason = deadline_waiter.WaitUntil(
            wake_at,
            capture.FrameAvailableEvent(),
            stop_event.Get(),
            wants_slot ? renderer.FrameLatencyWaitableObject() : nullptr);
        if (wait_reason == WaitReason::stop) {
            break;
        }
        if (wait_reason == WaitReason::slot) {
            renderer.MarkPresentationSlotAcquired();
        }
    }

    std::wcout << L"\nOSSS stopped.\n";
    return 0;
}

} // namespace

int wmain(const int argc, wchar_t** argv) {
    try {
        if (!osss::EnablePerMonitorV2DpiAwareness()) {
            throw std::runtime_error("OSSS could not enable per-monitor V2 DPI awareness.");
        }
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        Options options = ParseOptions(argc, argv);
        // Only on the capture path. The early-exit modes below have no
        // target to key `auto` off, and --help must never fail because a
        // window could not be resolved.
        const bool early_exit_mode = options.help || argc == 1 || options.list_windows ||
            options.self_test || options.warm_shader_cache || options.capture_self_test ||
            options.adaptive_capture_self_test;
        if (options.profile && !early_exit_mode) {
            const auto profiles = osss::LoadProfiles();
            if (!profiles.Ok()) {
                throw std::runtime_error(
                    "The profile file could not be read (line " +
                    std::to_string(profiles.error_line) + "): " +
                    winrt::to_string(profiles.error));
            }
            // `auto` keys off the target's executable, which is only knowable
            // once the target is resolved -- and resolving it needs the options
            // that are about to be re-parsed. Resolving twice is the honest way
            // out: it is a window-list lookup, it happens once at startup, and
            // the alternative is a settings struct that has to be merged by
            // hand and kept in sync with every flag.
            const bool automatic = *options.profile == L"auto";
            const std::wstring key = automatic
                ? TargetExecutableName(ResolveTarget(options))
                : *options.profile;
            const auto stored = osss::FindProfileArguments(profiles.entries, key);
            if (!stored && !automatic) {
                throw std::runtime_error(
                    "No profile is stored for " + winrt::to_string(*options.profile) + ".");
            }
            // Having no profile for this game is the ordinary case for `auto`,
            // not a failure: it means the defaults apply.
            if (stored) {
                // Profile arguments go first so an explicit flag on the real
                // command line overrides them: every parse branch assigns, so
                // the last occurrence of a flag is the one that survives.
                std::vector<std::wstring> merged(stored->begin(), stored->end());
                for (int index = 1; index < argc; ++index) {
                    merged.emplace_back(argv[index]);
                }
                std::vector<wchar_t*> pointers;
                pointers.push_back(argv[0]);
                for (std::wstring& argument : merged) {
                    pointers.push_back(argument.data());
                }
                options = ParseOptions(static_cast<int>(pointers.size()), pointers.data());
                std::wcout << L"Applied profile for " << key << L'\n';
            }
            options.profile.reset();
        }
        if (options.save_profile) {
            auto profiles = osss::LoadProfiles();
            if (!profiles.Ok()) {
                throw std::runtime_error(
                    "The profile file could not be read (line " +
                    std::to_string(profiles.error_line) + "): " +
                    winrt::to_string(profiles.error));
            }
            std::vector<std::wstring> stored;
            for (int index = 1; index < argc; ++index) {
                const std::wstring argument = argv[index];
                if (argument == L"--save-profile") {
                    ++index;
                    continue;
                }
                stored.push_back(argument);
            }
            osss::SetProfileArguments(profiles.entries, *options.save_profile, std::move(stored));
            std::wstring error;
            if (!osss::SaveProfiles(profiles.entries, error)) {
                throw std::runtime_error(winrt::to_string(error));
            }
            std::wcout << L"Saved profile for " << *options.save_profile << L" to "
                       << osss::ProfilePath().wstring() << L'\n';
            return 0;
        }
        if (options.help || argc == 1) {
            PrintUsage();
            return 0;
        }
        if (options.list_windows) {
            PrintWindows();
            return 0;
        }
        if (options.warm_shader_cache) {
            return RunWarmShaderCache();
        }
        if (options.self_test) {
            return RunSelfTest(
                options.flow_scale,
                options.performance_mode,
                options.output_mode,
                options.pacing_mode);
        }
        if (options.capture_self_test) {
            return osss::RunCaptureSmokeTest();
        }
        if (options.adaptive_capture_self_test) {
            return osss::RunAdaptiveCaptureSmokeTest();
        }
        return RunFrameGeneration(options);
    } catch (const winrt::hresult_error& error) {
        std::cerr
            << "Windows error 0x" << std::hex << static_cast<unsigned long>(error.code())
            << std::dec << ": " << winrt::to_string(error.message()) << '\n';
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
    }
    return 1;
}
