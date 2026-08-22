#include "dpi_awareness.h"
#include "png_writer.h"
#include "test_animation_backends.h"
#include "test_pattern.h"

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr wchar_t kWindowClassName[] = L"OSSS.TestAnimation";
constexpr wchar_t kFrameOutputClassName[] = L"OSSS.FrameOutput";
constexpr wchar_t kLauncherClassName[] = L"OSSS.SettingsLauncher";
constexpr double kCoarseSearchStepMilliseconds = 2.0;
constexpr double kFineSearchStepMilliseconds = 0.25;
constexpr double kSearchFutureMilliseconds = 25.0;
// Default burst interval as a fraction of the source frame period. The golden
// ratio conjugate is the classic low-discrepancy step: successive captures land
// at sub-frame phases that never repeat and spread as evenly as any sequence
// can, so a burst samples every generated-frame slot whatever the multiplier.
// A simple fraction such as 1/2 aliases against the output cadence instead --
// at 4x it visits only two of the four alphas, forever.
constexpr double kDefaultBurstIntervalSourcePeriods = 0.6180339887498949;
// A matched instant this close to k / base_fps is a real source frame. The
// fine temporal search steps by 0.25 ms, so a real frame lands within about
// 0.125 ms of the truth; the tightest generated spacing the harness supports
// (a 120 FPS source at 6x) is 1.39 ms.
constexpr double kRealFramePhaseToleranceMilliseconds = 0.5;

struct Options {
    osss::TestGraphicsApi api = osss::TestGraphicsApi::direct3d11;
    int base_fps = 60;
    double time_milliseconds = 0.0;
    double search_milliseconds = 250.0;
    double duration_seconds = 0.0;
    std::uint32_t burst_frames = 0;
    double burst_interval_milliseconds = 0.0; // 0 = kDefaultBurstIntervalSourcePeriods
    double burst_at_milliseconds = -1.0;      // negative = not scheduled from the CLI
    bool exit_after_burst = false;
    bool hud_overlay = false;
    bool help = false;
    bool self_test = false;
    std::optional<std::filesystem::path> export_reference;
    // Screen position to capture from, overriding the source window's client
    // origin. Needed to score a tool whose output is not drawn over the source
    // window at 1:1 -- see the head-to-head runbook in docs/TEST_ANIMATIONS.md.
    std::optional<POINT> capture_origin;
    std::optional<std::filesystem::path> compare_image;
};

struct WindowState {
    bool running = true;
    bool reset_requested = false;
    bool toggle_pause_requested = false;
    bool snapshot_requested = false;
    bool burst_requested = false;
    bool open_folder_requested = false;
};

// What, if anything, was covering the rectangle at the moment of a capture.
//
// This scorer reads the *screen*, which is the whole point -- it is how the same
// code can measure OSSS or a third-party generator without either cooperating.
// The cost is that it will just as happily score a terminal that happens to sit
// over the window. That is not a hypothetical: a run where OSSS failed to start
// reported 19 of 24 frames "generated", 9 backward steps, and a verdict of
// generated-frames-observed, entirely from capturing a console window. Every
// number in that report was meaningless and none of them looked wrong.
struct OcclusionCheck {
    bool occluded = false;
    // Executable of the covering window, for the report. Empty when clear.
    std::wstring blocker;
};

struct BurstFrame {
    double animation_seconds = 0.0;
    double capture_milliseconds = 0.0;
    OcclusionCheck occlusion{};
    std::vector<std::uint32_t> observed;
};

struct MatchResult {
    double expected_seconds = 0.0;
    double temporal_offset_milliseconds = 0.0;
    osss::TestPatternMetrics metrics{};
    std::vector<std::uint32_t> expected_pixels;
    // Scored separately from the moving content: a masked HUD holds one real
    // source-frame state, an interpolated one holds none.
    bool hud_overlay_checked = false;
    osss::HudOverlayCheck hud{};
};

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {
    }

    ~ScopedHandle() {
        if (handle_) {
            CloseHandle(handle_);
        }
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    [[nodiscard]] HANDLE Get() const noexcept {
        return handle_;
    }

private:
    HANDLE handle_ = nullptr;
};

class ScopedLauncherDisplacement {
public:
    ScopedLauncherDisplacement(
        const HWND test_window,
        const std::uint32_t width,
        const std::uint32_t height) {
        launcher_ = FindWindowW(kLauncherClassName, nullptr);
        if (!launcher_ || !IsWindowVisible(launcher_) || !GetWindowRect(launcher_, &original_bounds_)) {
            launcher_ = nullptr;
            return;
        }
        POINT client_origin{};
        if (!ClientToScreen(test_window, &client_origin)) {
            launcher_ = nullptr;
            return;
        }
        const RECT capture_bounds{
            client_origin.x,
            client_origin.y,
            client_origin.x + static_cast<LONG>(width),
            client_origin.y + static_cast<LONG>(height),
        };
        RECT overlap{};
        if (!IntersectRect(&overlap, &capture_bounds, &original_bounds_) || IsRectEmpty(&overlap)) {
            launcher_ = nullptr;
            return;
        }
        if (!SetWindowPos(
                launcher_,
                HWND_TOPMOST,
                -32000,
                -32000,
                0,
                0,
                SWP_NOSIZE | SWP_NOACTIVATE)) {
            launcher_ = nullptr;
            return;
        }
        DwmFlush();
    }

    ~ScopedLauncherDisplacement() {
        if (!launcher_) {
            return;
        }
        SetWindowPos(
            launcher_,
            HWND_TOPMOST,
            original_bounds_.left,
            original_bounds_.top,
            0,
            0,
            SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    ScopedLauncherDisplacement(const ScopedLauncherDisplacement&) = delete;
    ScopedLauncherDisplacement& operator=(const ScopedLauncherDisplacement&) = delete;

private:
    HWND launcher_ = nullptr;
    RECT original_bounds_{};
};

std::wstring RequireValue(const int argc, wchar_t** argv, int& index, const wchar_t* option) {
    if (index + 1 >= argc) {
        throw std::invalid_argument(std::string("Missing value for ") +
            std::filesystem::path(option).string() + ".");
    }
    return argv[++index];
}

int ParseInteger(const std::wstring& value, const wchar_t* option) {
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(value, &consumed);
        if (consumed == value.size()) {
            return parsed;
        }
    } catch (const std::exception&) {
    }
    throw std::invalid_argument(std::filesystem::path(option).string() + " requires an integer.");
}

double ParseNumber(const std::wstring& value, const wchar_t* option) {
    try {
        std::size_t consumed = 0;
        const double parsed = std::stod(value, &consumed);
        if (consumed == value.size() && std::isfinite(parsed)) {
            return parsed;
        }
    } catch (const std::exception&) {
    }
    throw std::invalid_argument(std::filesystem::path(option).string() + " requires a number.");
}

Options ParseOptions(const int argc, wchar_t** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument = argv[index];
        if (argument == L"--help" || argument == L"-h") {
            options.help = true;
        } else if (argument == L"--self-test") {
            options.self_test = true;
        } else if (argument == L"--api") {
            const std::wstring value = RequireValue(argc, argv, index, L"--api");
            const auto api = osss::ParseTestGraphicsApi(value);
            if (!api) {
                throw std::invalid_argument("--api must be d3d9, d3d10, d3d11, or d3d12.");
            }
            options.api = *api;
        } else if (argument == L"--fps") {
            options.base_fps = ParseInteger(RequireValue(argc, argv, index, L"--fps"), L"--fps");
        } else if (argument == L"--time-ms") {
            options.time_milliseconds = ParseNumber(
                RequireValue(argc, argv, index, L"--time-ms"),
                L"--time-ms");
        } else if (argument == L"--search-ms") {
            options.search_milliseconds = ParseNumber(
                RequireValue(argc, argv, index, L"--search-ms"),
                L"--search-ms");
        } else if (argument == L"--duration-seconds") {
            options.duration_seconds = ParseNumber(
                RequireValue(argc, argv, index, L"--duration-seconds"),
                L"--duration-seconds");
        } else if (argument == L"--burst") {
            const int frames = ParseInteger(RequireValue(argc, argv, index, L"--burst"), L"--burst");
            if (frames < 1 || frames > 600) {
                throw std::invalid_argument("--burst must be from 1 through 600 frames.");
            }
            options.burst_frames = static_cast<std::uint32_t>(frames);
        } else if (argument == L"--burst-interval-ms") {
            options.burst_interval_milliseconds = ParseNumber(
                RequireValue(argc, argv, index, L"--burst-interval-ms"),
                L"--burst-interval-ms");
        } else if (argument == L"--burst-at-ms") {
            options.burst_at_milliseconds = ParseNumber(
                RequireValue(argc, argv, index, L"--burst-at-ms"),
                L"--burst-at-ms");
        } else if (argument == L"--exit-after-burst") {
            options.exit_after_burst = true;
        } else if (argument == L"--hud-overlay") {
            options.hud_overlay = true;
        } else if (argument == L"--export-reference") {
            options.export_reference = RequireValue(argc, argv, index, L"--export-reference");
        } else if (argument == L"--capture-origin") {
            const std::wstring value = RequireValue(argc, argv, index, L"--capture-origin");
            const std::size_t separator = value.find(L',');
            if (separator == std::wstring::npos) {
                throw std::invalid_argument("--capture-origin expects X,Y in screen pixels.");
            }
            POINT captured{};
            captured.x = ParseInteger(value.substr(0, separator), L"--capture-origin");
            captured.y = ParseInteger(value.substr(separator + 1), L"--capture-origin");
            options.capture_origin = captured;
        } else if (argument == L"--compare") {
            options.compare_image = RequireValue(argc, argv, index, L"--compare");
        } else {
            throw std::invalid_argument(
                "Unknown argument: " + std::filesystem::path(argument).string());
        }
    }
    if (!osss::IsTestAnimationBaseRate(options.base_fps)) {
        throw std::invalid_argument("--fps must be 10, 20, 30, ..., 110, or 120.");
    }
    if (options.time_milliseconds < 0.0 || options.search_milliseconds < 0.0 ||
        options.duration_seconds < 0.0 || options.burst_interval_milliseconds < 0.0) {
        throw std::invalid_argument("Time, search, duration, and burst interval values must not be negative.");
    }
    if (options.burst_at_milliseconds >= 0.0 && options.burst_frames == 0) {
        throw std::invalid_argument("--burst-at-ms requires --burst <frames>.");
    }
    if (options.exit_after_burst && options.burst_at_milliseconds < 0.0) {
        throw std::invalid_argument("--exit-after-burst requires --burst-at-ms.");
    }
    if (options.export_reference && options.compare_image) {
        throw std::invalid_argument("Choose --export-reference or --compare, not both.");
    }
    return options;
}

void PrintUsage() {
    std::wcout
        << L"OSSS deterministic DirectX test animation\n\n"
        << L"Interactive:\n"
        << L"  osss_test_animation --api d3d9|d3d10|d3d11|d3d12 --fps 10..120\n\n"
        << L"Reference and comparison:\n"
        << L"  osss_test_animation --api d3d11 --fps 60 --time-ms 1250 "
           L"--export-reference expected.ppm\n"
        << L"  osss_test_animation --api d3d11 --fps 60 --time-ms 1250 "
           L"--compare observed.ppm [--search-ms 250]\n\n"
           L"--capture-origin X,Y\n"
           L"    Capture from this screen position instead of the source window's\n"
           L"    client origin. Use it to score a frame generator whose output is not\n"
           L"    drawn over the source at 1:1 -- see docs/TEST_ANIMATIONS.md.\n\n"
        << L"Burst (consecutive-frame) scoring:\n"
        << L"  osss_test_animation --api d3d11 --fps 60 --burst 10 [--burst-interval-ms 10.3]\n"
        << L"      [--burst-at-ms 1000 [--exit-after-burst]]\n"
        << L"  Without --burst-at-ms, press B in the window to capture a burst.\n"
        << L"  The interval defaults to 0.618 source frame periods so successive\n"
        << L"  captures sweep every generated-frame phase; a simple fraction of the\n"
        << L"  period aliases against the output cadence. Each unique frame is\n"
        << L"  classified real or generated from where its best-matched instant\n"
        << L"  falls against the source frame grid, and the summary states a verdict.\n\n"
        << L"HUD overlay (UI-mask fixture):\n"
        << L"  osss_test_animation --api d3d11 --fps 60 --hud-overlay\n"
        << L"  Adds two static-position HUD panels whose cells step only at source\n"
        << L"  frame boundaries. Interpolating them produces colours no real frame\n"
        << L"  contains, so they are a direct test of OSSS --ui-mask. The panels are\n"
        << L"  excluded from the moving-content score and reported separately. The\n"
        << L"  matching mask argument is printed at startup.\n\n"
        << L"Other:\n"
        << L"  --duration-seconds <seconds>  Close automatically after that much\n"
           L"                                animation has elapsed. 0 (default) runs\n"
           L"                                until the window is closed.\n"
        << L"  --self-test                   Run the built-in reference checks and\n"
           L"                                exit. No window is created.\n\n"
        << L"Interactive keys: S scores a screen snapshot, B captures a burst, O opens the\n"
        << L"evidence folder, R resets the six-second diagnostic cycle, Space pauses, and\n"
        << L"Esc closes.\n";
}

std::string NarrowAscii(const std::wstring_view value) {
    std::string output;
    output.reserve(value.size());
    for (const wchar_t character : value) {
        output.push_back(character >= 0 && character <= 0x7F ? static_cast<char>(character) : '?');
    }
    return output;
}

std::string JsonEscape(const std::string_view value) {
    std::string output;
    output.reserve(value.size() + 8);
    for (const char character : value) {
        switch (character) {
        case '\\':
            output += "\\\\";
            break;
        case '"':
            output += "\\\"";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        default:
            output.push_back(character);
            break;
        }
    }
    return output;
}

std::wstring BaseTitle(const Options& options) {
    return L"OSSS Test Animation | " +
        std::wstring(osss::TestGraphicsApiDisplayName(options.api)) + L" | " +
        std::to_wstring(options.base_fps) +
        L" FPS | S score | B burst | O evidence | R reset | Space pause | Esc close";
}

LRESULT CALLBACK WindowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<WindowState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_KEYDOWN:
        if (!state) {
            break;
        }
        switch (wparam) {
        case 'S':
            state->snapshot_requested = true;
            return 0;
        case 'B':
            state->burst_requested = true;
            return 0;
        case 'O':
            state->open_folder_requested = true;
            return 0;
        case 'R':
            state->reset_requested = true;
            return 0;
        case VK_SPACE:
            state->toggle_pause_requested = true;
            return 0;
        case VK_ESCAPE:
            DestroyWindow(window);
            return 0;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (state) {
            state->running = false;
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool RegisterWindowClass(const HINSTANCE instance) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_OWNDC;
    window_class.lpfnWndProc = WindowProcedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    window_class.hIconSm = window_class.hIcon;
    window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = kWindowClassName;
    return RegisterClassExW(&window_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND CreateAnimationWindow(
    const HINSTANCE instance,
    WindowState& state,
    const Options& options,
    const std::uint32_t width,
    const std::uint32_t height) {
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT bounds{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRectEx(&bounds, style, FALSE, 0);
    return CreateWindowExW(
        0,
        kWindowClassName,
        BaseTitle(options).c_str(),
        style,
        32,
        32,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        nullptr,
        nullptr,
        instance,
        &state);
}

std::filesystem::path CreateSessionDirectory(const Options& options) {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::wostringstream name;
    name << std::setfill(L'0')
         << std::setw(4) << time.wYear
         << std::setw(2) << time.wMonth
         << std::setw(2) << time.wDay << L'-'
         << std::setw(2) << time.wHour
         << std::setw(2) << time.wMinute
         << std::setw(2) << time.wSecond << L'-'
         << osss::TestGraphicsApiArgument(options.api) << L'-'
         << options.base_fps << L"fps-p" << GetCurrentProcessId();
    const auto directory = std::filesystem::temp_directory_path() /
        L"OSSS" / L"test-captures" / name.str();
    std::filesystem::create_directories(directory);
    return directory;
}

void WriteSessionManifest(
    const std::filesystem::path& directory,
    const Options& options,
    const std::int64_t qpc_frequency) {
    std::ofstream output(directory / "manifest.json");
    output
        << "{\n"
        << "  \"schema\": \"osss-test-animation-v1\",\n"
        << "  \"api\": \"" << NarrowAscii(osss::TestGraphicsApiArgument(options.api)) << "\",\n"
        << "  \"base_fps\": " << options.base_fps << ",\n"
        << "  \"width\": " << osss::TestPatternSpec::kDefaultWidth << ",\n"
        << "  \"height\": " << osss::TestPatternSpec::kDefaultHeight << ",\n"
        << "  \"scored_top\": " << osss::TestPatternSpec::kScoredTop << ",\n"
        << "  \"cycle_seconds\": " << osss::TestPatternSpec::kCycleSeconds << ",\n"
        << "  \"qpc_frequency\": " << qpc_frequency << ",\n"
        << "  \"patterns\": [\"constant-velocity-translation\", "
           "\"occlusion-disocclusion\", \"thin-detail-phase\", \"hard-scene-cut\"],\n"
        << "  \"metric_note\": \"MAE, RMSE, PSNR, and pixels over 8/255 are scored below the machine header after temporal best-fit alignment.\"\n"
        << "}\n";
}

// Hit-tests a grid across the capture rectangle and reports the first window
// found there that is neither the target nor another window of the target's
// process.
//
// A frame generator's overlay does not register: OSSS's output window is
// WS_EX_TRANSPARENT so that input passes through to the game, and
// WindowFromPoint skips click-through windows by design. That is exactly the
// behaviour wanted here -- an overlay drawn over the target is the thing being
// measured, while a terminal parked on top of it is not.
OcclusionCheck CheckCaptureOcclusion(
    const HWND window,
    const POINT origin,
    const std::uint32_t width,
    const std::uint32_t height) {
    OcclusionCheck check;
    DWORD target_process = 0;
    GetWindowThreadProcessId(window, &target_process);

    // A 5x5 grid inset from the edges. Enough to catch a window covering any
    // meaningful part of the pattern without hit-testing thousands of points;
    // the insets keep the samples off the border, where a one-pixel overlap
    // with a neighbouring window would be a false positive.
    constexpr int kSamples = 5;
    for (int row = 0; row < kSamples; ++row) {
        for (int column = 0; column < kSamples; ++column) {
            POINT point{
                origin.x + static_cast<LONG>(
                    (static_cast<std::uint64_t>(width) * (2 * column + 1)) / (2 * kSamples)),
                origin.y + static_cast<LONG>(
                    (static_cast<std::uint64_t>(height) * (2 * row + 1)) / (2 * kSamples))};
            const HWND at = WindowFromPoint(point);
            if (!at) {
                continue;
            }
            const HWND root = GetAncestor(at, GA_ROOT);
            if (root == window) {
                continue;
            }
            DWORD process = 0;
            GetWindowThreadProcessId(root, &process);
            if (process == target_process) {
                continue;
            }
            check.occluded = true;
            check.blocker = L"unknown";
            if (const HANDLE handle =
                    OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process)) {
                std::wstring path(MAX_PATH, L'\0');
                DWORD length = static_cast<DWORD>(path.size());
                const BOOL queried = QueryFullProcessImageNameW(handle, 0, path.data(), &length);
                CloseHandle(handle);
                if (queried && length > 0) {
                    path.resize(length);
                    const std::size_t separator = path.find_last_of(L"\\/");
                    check.blocker =
                        separator == std::wstring::npos ? path : path.substr(separator + 1);
                }
            }
            return check;
        }
    }
    return check;
}

std::vector<std::uint32_t> CaptureClientPixels(
    const HWND window,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::optional<POINT> origin_override,
    OcclusionCheck* const occlusion = nullptr) {
    // GetDC(nullptr) below reads the *screen*, so this captures whatever is
    // actually composited at the rectangle -- the source window when nothing
    // covers it, and a frame generator's output when one does. That is what lets
    // the same scorer measure OSSS and a third-party tool on identical input.
    POINT origin{};
    if (origin_override) {
        origin = *origin_override;
    } else if (!ClientToScreen(window, &origin)) {
        throw std::runtime_error("ClientToScreen failed while capturing a score snapshot.");
    }
    // Checked before the BitBlt, not after: what covers the rectangle at the
    // moment the pixels are read is what ends up in them.
    if (occlusion) {
        *occlusion = CheckCaptureOcclusion(window, origin, width, height);
    }
    HDC screen = GetDC(nullptr);
    if (!screen) {
        throw std::runtime_error("GetDC failed while capturing a score snapshot.");
    }
    HDC memory = CreateCompatibleDC(screen);
    if (!memory) {
        ReleaseDC(nullptr, screen);
        throw std::runtime_error("CreateCompatibleDC failed while capturing a score snapshot.");
    }

    BITMAPINFO information{};
    information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    information.bmiHeader.biWidth = static_cast<LONG>(width);
    information.bmiHeader.biHeight = -static_cast<LONG>(height);
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    information.bmiHeader.biCompression = BI_RGB;
    void* bitmap_pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(
        screen,
        &information,
        DIB_RGB_COLORS,
        &bitmap_pixels,
        nullptr,
        0);
    if (!bitmap || !bitmap_pixels) {
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        throw std::runtime_error("CreateDIBSection failed while capturing a score snapshot.");
    }
    const HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
    const BOOL copied = BitBlt(
        memory,
        0,
        0,
        static_cast<int>(width),
        static_cast<int>(height),
        screen,
        origin.x,
        origin.y,
        SRCCOPY | CAPTUREBLT);
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height);
    if (copied) {
        const auto* source = static_cast<const std::uint32_t*>(bitmap_pixels);
        for (std::size_t index = 0; index < pixels.size(); ++index) {
            pixels[index] = source[index] | 0xFF000000U;
        }
    }
    SelectObject(memory, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    if (!copied) {
        throw std::runtime_error("BitBlt failed while capturing a score snapshot.");
    }
    return pixels;
}

MatchResult FindBestMatch(
    const osss::TestPatternSpec& specification,
    const std::span<const std::uint32_t> observed,
    const double around_seconds,
    const double search_milliseconds) {
    double best_seconds = std::max(0.0, around_seconds - search_milliseconds / 1000.0);
    double best_error = std::numeric_limits<double>::infinity();
    const double last_seconds = around_seconds + kSearchFutureMilliseconds / 1000.0;
    // A masked HUD deliberately shows a different source frame than the
    // interpolated content around it, so it must not steer the time search.
    const auto hud_rects = osss::TestPatternHudRects(specification);
    for (double candidate = best_seconds;
         candidate <= last_seconds + 0.0000001;
         candidate += kCoarseSearchStepMilliseconds / 1000.0) {
        const auto expected = osss::RenderTestPattern(
            specification,
            candidate,
            static_cast<std::uint64_t>(std::floor(candidate * specification.base_fps)));
        const auto metrics = osss::CompareTestPatternFrames(
            expected,
            observed,
            specification.width,
            specification.height,
            osss::TestPatternSpec::kScoredTop,
            8,
            4,
            hud_rects);
        if (metrics.mean_absolute_error < best_error) {
            best_error = metrics.mean_absolute_error;
            best_seconds = candidate;
        }
    }

    const double fine_first = std::max(0.0, best_seconds - 0.0025);
    const double fine_last = best_seconds + 0.0025;
    for (double candidate = fine_first;
         candidate <= fine_last + 0.0000001;
         candidate += kFineSearchStepMilliseconds / 1000.0) {
        const auto expected = osss::RenderTestPattern(
            specification,
            candidate,
            static_cast<std::uint64_t>(std::floor(candidate * specification.base_fps)));
        const auto metrics = osss::CompareTestPatternFrames(
            expected,
            observed,
            specification.width,
            specification.height,
            osss::TestPatternSpec::kScoredTop,
            8,
            2,
            hud_rects);
        if (metrics.mean_absolute_error < best_error) {
            best_error = metrics.mean_absolute_error;
            best_seconds = candidate;
        }
    }

    MatchResult result;
    result.expected_seconds = best_seconds;
    result.temporal_offset_milliseconds = (best_seconds - around_seconds) * 1000.0;
    result.expected_pixels = osss::RenderTestPattern(
        specification,
        best_seconds,
        static_cast<std::uint64_t>(std::floor(best_seconds * specification.base_fps)));
    result.metrics = osss::CompareTestPatternFrames(
        result.expected_pixels,
        observed,
        specification.width,
        specification.height,
        osss::TestPatternSpec::kScoredTop,
        8,
        1,
        hud_rects);

    if (!hud_rects.empty()) {
        // Allow the bracketing pair on either side of the matched time, since a
        // masked HUD legitimately holds the newer or older real frame.
        const auto centre = static_cast<std::int64_t>(
            std::floor(best_seconds * specification.base_fps));
        const auto first = static_cast<std::uint64_t>(std::max<std::int64_t>(0, centre - 2));
        result.hud = osss::CheckTestPatternHud(
            observed,
            specification,
            first,
            static_cast<std::uint64_t>(centre + 2));
        result.hud_overlay_checked = true;
    }
    return result;
}

bool FrameOutputIsVisibleOver(const HWND test_window) {
    const HWND output = FindWindowW(kFrameOutputClassName, nullptr);
    if (!output || !IsWindowVisible(output)) {
        return false;
    }
    RECT output_bounds{};
    RECT test_bounds{};
    RECT overlap{};
    return GetWindowRect(output, &output_bounds) && GetWindowRect(test_window, &test_bounds) &&
        IntersectRect(&overlap, &output_bounds, &test_bounds) &&
        !IsRectEmpty(&overlap);
}

void WriteSnapshotReport(
    const std::filesystem::path& path,
    const Options& options,
    const double capture_seconds,
    const MatchResult& match,
    const bool frame_output_visible,
    const std::filesystem::path& observed_path,
    const std::filesystem::path& expected_path) {
    std::ofstream output(path);
    output << std::fixed << std::setprecision(6)
        << "{\n"
        << "  \"schema\": \"osss-test-animation-score-v1\",\n"
        << "  \"api\": \"" << NarrowAscii(osss::TestGraphicsApiArgument(options.api)) << "\",\n"
        << "  \"base_fps\": " << options.base_fps << ",\n"
        << "  \"osss_frame_output_visible\": " << (frame_output_visible ? "true" : "false") << ",\n"
        << "  \"capture_animation_seconds\": " << capture_seconds << ",\n"
        << "  \"best_expected_seconds\": " << match.expected_seconds << ",\n"
        << "  \"temporal_offset_ms\": " << match.temporal_offset_milliseconds << ",\n"
        << "  \"mae\": " << match.metrics.mean_absolute_error << ",\n"
        << "  \"rmse\": " << match.metrics.root_mean_square_error << ",\n"
        << "  \"psnr_db\": ";
    if (std::isinf(match.metrics.psnr_db)) {
        output << "null";
    } else {
        output << match.metrics.psnr_db;
    }
    output
        << ",\n  \"pixels_over_8_percent\": "
        << match.metrics.pixels_over_threshold_percent << ",\n"
        << "  \"maximum_channel_error\": "
        << static_cast<unsigned>(match.metrics.maximum_channel_error) << ",\n"
        << "  \"compared_pixels\": " << match.metrics.compared_pixels << ",\n";
    if (match.hud_overlay_checked) {
        output
            << "  \"hud_overlay\": {\n"
            << "    \"excluded_from_pixel_metrics\": true,\n"
            << "    \"holds_one_real_source_frame\": "
            << (match.hud.matches_discrete_state ? "true" : "false") << ",\n"
            << "    \"matched_source_frame_index\": "
            << match.hud.matched_source_frame_index << ",\n"
            << "    \"maximum_channel_error_to_nearest_state\": "
            << static_cast<unsigned>(match.hud.maximum_channel_error) << ",\n"
            << "    \"compared_pixels\": " << match.hud.compared_pixels << ",\n"
            << "    \"interpretation\": \"A masked HUD equals exactly one real source frame. A large error means the HUD was interpolated into a state no source frame contained.\"\n"
            << "  },\n";
    }
    output
        << "  \"observed_ppm\": \"" << JsonEscape(observed_path.string()) << "\",\n"
        << "  \"expected_ppm\": \"" << JsonEscape(expected_path.string()) << "\",\n"
        << "  \"interpretation\": \"Pixel metrics are best-aligned image variance; temporal_offset_ms is measured separately and negative values mean the displayed frame trails the source clock.\"\n"
        << "}\n";
}

std::wstring ScoreTitle(const Options& options, const MatchResult& match, const bool output_visible) {
    std::wostringstream title;
    title << BaseTitle(options) << L" | " << (output_visible ? L"OSSS" : L"native") << L" score: ";
    if (std::isinf(match.metrics.psnr_db)) {
        title << L"exact";
    } else {
        title << std::fixed << std::setprecision(1) << match.metrics.psnr_db << L" dB";
    }
    title << L", " << std::showpos << std::fixed << std::setprecision(1)
          << match.temporal_offset_milliseconds << L" ms";
    if (match.hud_overlay_checked) {
        title << std::noshowpos << L" | HUD "
              << (match.hud.matches_discrete_state
                  ? L"clean"
                  : L"interpolated (err " +
                        std::to_wstring(match.hud.maximum_channel_error) + L")");
    }
    return title.str();
}

// Writes a PNG beside a PPM that has just been written. The PPM stays because
// --compare reads it; the PNG is what a person or a viewer can open, and
// evidence nobody can look at has historically gone unlooked-at.
void WritePngBeside(
    const std::filesystem::path& ppm_path,
    const std::span<const std::uint32_t> pixels,
    const osss::TestPatternSpec& specification) {
    std::filesystem::path png_path = ppm_path;
    png_path.replace_extension(".png");
    std::string error;
    osss::WritePng(png_path, pixels, specification.width, specification.height, error);
}

void SaveAndScoreSnapshot(
    const HWND window,
    const Options& options,
    const osss::TestPatternSpec& specification,
    const std::filesystem::path& session_directory,
    const std::uint64_t snapshot_index,
    const double animation_seconds) {
    std::vector<std::uint32_t> observed;
    OcclusionCheck occlusion;
    {
        const ScopedLauncherDisplacement displacement(
            window,
            specification.width,
            specification.height);
        observed = CaptureClientPixels(
            window,
            specification.width,
            specification.height,
            options.capture_origin,
            &occlusion);
    }
    if (occlusion.occluded) {
        // Same failure as the burst, one frame instead of many: scoring a
        // window that is not the source produces a number, and the number is
        // about the wrong window. Refuse rather than write a plausible report.
        std::wostringstream message;
        message << BaseTitle(options) << L" | OCCLUDED by " << occlusion.blocker
                << L" -- snapshot not scored";
        SetWindowTextW(window, message.str().c_str());
        std::cout << "snapshot skipped: the capture rectangle was covered by "
                  << NarrowAscii(occlusion.blocker)
                  << ". Uncover the window and press S again.\n";
        return;
    }
    const MatchResult match = FindBestMatch(
        specification,
        observed,
        animation_seconds,
        options.search_milliseconds);
    const auto stem = std::string("snapshot-") + std::to_string(snapshot_index);
    const auto observed_path = session_directory / (stem + "-observed.ppm");
    const auto expected_path = session_directory / (stem + "-expected.ppm");
    const auto report_path = session_directory / (stem + "-report.json");
    std::string error;
    if (!osss::WriteTestPatternPpm(
            observed_path,
            observed,
            specification.width,
            specification.height,
            error) ||
        !osss::WriteTestPatternPpm(
            expected_path,
            match.expected_pixels,
            specification.width,
            specification.height,
            error)) {
        throw std::runtime_error(error);
    }
    WritePngBeside(observed_path, observed, specification);
    WritePngBeside(expected_path, match.expected_pixels, specification);
    const bool output_visible = FrameOutputIsVisibleOver(window);
    WriteSnapshotReport(
        report_path,
        options,
        animation_seconds,
        match,
        output_visible,
        observed_path,
        expected_path);
    SetWindowTextW(window, ScoreTitle(options, match, output_visible).c_str());
}

void WriteJsonNumberOrNull(std::ostream& output, const double value) {
    if (std::isinf(value)) {
        output << "null";
    } else {
        output << value;
    }
}

// Scores every frame of a completed burst against the analytic ground truth,
// writes observed/expected PPMs plus one JSON document with per-frame rows and
// a sequence summary, and returns the summary line shown in the title bar.
std::wstring ScoreAndWriteBurst(
    const Options& options,
    const osss::TestPatternSpec& specification,
    const std::filesystem::path& session_directory,
    const std::uint64_t burst_index,
    const std::vector<BurstFrame>& frames,
    const std::uint32_t requested_frames,
    const double interval_milliseconds,
    const bool output_visible) {
    struct Row {
        MatchResult match;
        bool identical_to_previous = false;
        // Where the best-matched instant falls against the source frame grid.
        // A real frame -- captured from the source, or held or re-presented by
        // OSSS -- sits on the grid; a generated frame sits between two lines.
        double source_phase_milliseconds = 0.0;
        bool generated = false;
        std::filesystem::path observed_path;
        std::filesystem::path expected_path;
        // Scored per lane as well as whole-frame. A whole-frame mean is enough
        // to say two tools differ and not enough to say how: the lanes fail in
        // different ways, and thin detail in particular can collapse while the
        // mean barely moves.
        osss::TestPatternMetrics lanes[std::size(osss::kTestPatternLanes)]{};
    };
    const auto prefix = std::string("burst-") + std::to_string(burst_index);
    const auto scoring_started = std::chrono::steady_clock::now();

    // Each frame's temporal search is an independent, pure CPU scan over the
    // analytic pattern, and it is by far the most expensive part of a burst.
    // Running it serially on the message-loop thread stalls source presentation
    // for seconds, so fan the searches out and keep the search itself unchanged.
    std::vector<std::future<MatchResult>> pending;
    pending.reserve(frames.size());
    for (const auto& frame : frames) {
        pending.push_back(std::async(
            std::launch::async,
            [&specification, &frame, &options] {
                return FindBestMatch(
                    specification,
                    frame.observed,
                    frame.animation_seconds,
                    options.search_milliseconds);
            }));
    }

    std::vector<Row> rows;
    rows.reserve(frames.size());
    for (std::size_t index = 0; index < frames.size(); ++index) {
        Row row;
        row.match = pending[index].get();
        row.identical_to_previous = index > 0 && frames[index].observed == frames[index - 1].observed;
        row.source_phase_milliseconds = osss::SourceFramePhaseMilliseconds(
            row.match.expected_seconds,
            specification.base_fps);
        row.generated = !osss::IsSourceFrameInstant(
            row.match.expected_seconds,
            specification.base_fps,
            kRealFramePhaseToleranceMilliseconds);
        for (std::size_t lane = 0; lane < std::size(osss::kTestPatternLanes); ++lane) {
            const auto& bounds = osss::kTestPatternLanes[lane];
            // Isolate the band: score from its top, exclude everything below it.
            const osss::TestPatternRect below{
                0,
                bounds.bottom,
                specification.width,
                specification.height};
            row.lanes[lane] = osss::CompareTestPatternFrames(
                row.match.expected_pixels,
                frames[index].observed,
                specification.width,
                specification.height,
                bounds.top,
                8,
                1,
                std::span<const osss::TestPatternRect>(&below, 1));
        }
        const auto stem = prefix + "-frame-" + std::to_string(index);
        row.observed_path = session_directory / (stem + "-observed.ppm");
        row.expected_path = session_directory / (stem + "-expected.ppm");
        std::string error;
        if (!osss::WriteTestPatternPpm(
                row.observed_path,
                frames[index].observed,
                specification.width,
                specification.height,
                error) ||
            !osss::WriteTestPatternPpm(
                row.expected_path,
                row.match.expected_pixels,
                specification.width,
                specification.height,
                error)) {
            throw std::runtime_error(error);
        }
        WritePngBeside(row.observed_path, frames[index].observed, specification);
        WritePngBeside(row.expected_path, row.match.expected_pixels, specification);
        rows.push_back(std::move(row));
    }

    // Sequence summary. "Unique" frames are those whose pixels differ from the
    // previous capture; only they say anything about presentation cadence.
    //
    // The real/generated split is the burst's answer to a question per-frame
    // PSNR cannot ask. A held or re-presented real frame is a perfect match at
    // *some* instant, so a session that never generated anything scores as
    // well as one that did; only the phase of the matched instants tells the
    // two apart. Every classified frame is unique, so a re-captured generated
    // frame is counted once.
    std::size_t unique_frames = 0;
    std::size_t generated_frames = 0;
    std::size_t backward_steps = 0;
    double psnr_min = std::numeric_limits<double>::infinity();
    double psnr_max = -std::numeric_limits<double>::infinity();
    double psnr_sum = 0.0;
    std::size_t psnr_count = 0;
    double offset_min = std::numeric_limits<double>::infinity();
    double offset_max = -std::numeric_limits<double>::infinity();
    double offset_sum = 0.0;
    double expected_step_min = std::numeric_limits<double>::infinity();
    double expected_step_max = -std::numeric_limits<double>::infinity();
    unsigned worst_channel_error = 0;
    double previous_unique_expected = -1.0;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        const bool unique = index == 0 || !row.identical_to_previous;
        if (unique) {
            ++unique_frames;
            if (row.generated) {
                ++generated_frames;
            }
            if (previous_unique_expected >= 0.0) {
                const double step = (row.match.expected_seconds - previous_unique_expected) * 1000.0;
                expected_step_min = std::min(expected_step_min, step);
                expected_step_max = std::max(expected_step_max, step);
                if (step < 0.0) {
                    ++backward_steps;
                }
            }
            previous_unique_expected = row.match.expected_seconds;
        }
        const double psnr = row.match.metrics.psnr_db;
        if (!std::isinf(psnr)) {
            psnr_min = std::min(psnr_min, psnr);
            psnr_max = std::max(psnr_max, psnr);
            psnr_sum += psnr;
            ++psnr_count;
        }
        offset_min = std::min(offset_min, row.match.temporal_offset_milliseconds);
        offset_max = std::max(offset_max, row.match.temporal_offset_milliseconds);
        offset_sum += row.match.temporal_offset_milliseconds;
        worst_channel_error = std::max<unsigned>(worst_channel_error, row.match.metrics.maximum_channel_error);
    }
    const double capture_span_ms = frames.empty()
        ? 0.0
        : (frames.back().animation_seconds - frames.front().animation_seconds) * 1000.0;
    const std::size_t real_frames = unique_frames - generated_frames;
    // One unique frame means the desktop never changed during the burst --
    // paused source, capture-bound interval, or a stalled display path -- and
    // says nothing either way.
    // Split because they answer different questions. Real frames passing
    // through at native fidelity says the capture and present path are clean;
    // generated frames are the only ones that measure the interpolator.
    struct LaneTotals {
        double generated_psnr = 0.0;
        double generated_bad_percent = 0.0;
        std::size_t generated_count = 0;
        double real_psnr = 0.0;
        std::size_t real_count = 0;
    };
    LaneTotals lane_totals[std::size(osss::kTestPatternLanes)]{};
    for (const Row& lane_row : rows) {
        for (std::size_t lane = 0; lane < std::size(osss::kTestPatternLanes); ++lane) {
            const double psnr = lane_row.lanes[lane].psnr_db;
            if (!std::isfinite(psnr)) {
                continue;
            }
            if (lane_row.generated) {
                lane_totals[lane].generated_psnr += psnr;
                lane_totals[lane].generated_bad_percent +=
                    lane_row.lanes[lane].pixels_over_threshold_percent;
                ++lane_totals[lane].generated_count;
            } else {
                lane_totals[lane].real_psnr += psnr;
                ++lane_totals[lane].real_count;
            }
        }
    }

    // Occlusion outranks every other verdict. A covered capture does not
    // produce a wrong-but-related answer, it produces pixels from an unrelated
    // window that the temporal search then matches to whatever instant fits
    // least badly -- which reads as fast motion, so the frames classify as
    // *generated* and the run looks like its best one. Saying "occluded" and
    // refusing the rest is the only honest report.
    std::size_t occluded_frames = 0;
    std::wstring blocker;
    for (const BurstFrame& frame : frames) {
        if (frame.occlusion.occluded) {
            ++occluded_frames;
            if (blocker.empty()) {
                blocker = frame.occlusion.blocker;
            }
        }
    }

    const char* verdict = occluded_frames > 0
        ? "occluded"
        : (generated_frames > 0
               ? "generated-frames-observed"
               : (unique_frames >= 2 ? "source-frames-only" : "inconclusive"));

    const auto report_path = session_directory / (prefix + ".json");
    std::ofstream output(report_path);
    output << std::fixed << std::setprecision(6)
        << "{\n"
        << "  \"schema\": \"osss-test-animation-burst-v1\",\n"
        << "  \"api\": \"" << NarrowAscii(osss::TestGraphicsApiArgument(options.api)) << "\",\n"
        << "  \"base_fps\": " << options.base_fps << ",\n"
        << "  \"osss_frame_output_visible\": " << (output_visible ? "true" : "false") << ",\n"
        << "  \"requested_frames\": " << requested_frames << ",\n"
        << "  \"requested_interval_ms\": " << interval_milliseconds << ",\n"
        << "  \"summary\": {\n"
        << "    \"captured_frames\": " << rows.size() << ",\n"
        << "    \"unique_frames\": " << unique_frames << ",\n"
        << "    \"capture_span_ms\": " << capture_span_ms << ",\n"
        << "    \"psnr_db_min\": ";
    WriteJsonNumberOrNull(output, psnr_count ? psnr_min : std::numeric_limits<double>::infinity());
    output << ",\n    \"psnr_db_mean\": ";
    WriteJsonNumberOrNull(output, psnr_count ? psnr_sum / static_cast<double>(psnr_count) : std::numeric_limits<double>::infinity());
    output << ",\n    \"psnr_db_max\": ";
    WriteJsonNumberOrNull(output, psnr_count ? psnr_max : std::numeric_limits<double>::infinity());
    output
        << ",\n    \"temporal_offset_ms_min\": " << (rows.empty() ? 0.0 : offset_min) << ",\n"
        << "    \"temporal_offset_ms_mean\": " << (rows.empty() ? 0.0 : offset_sum / static_cast<double>(rows.size())) << ",\n"
        << "    \"temporal_offset_ms_max\": " << (rows.empty() ? 0.0 : offset_max) << ",\n"
        << "    \"expected_step_ms_min\": " << (unique_frames > 1 ? expected_step_min : 0.0) << ",\n"
        << "    \"expected_step_ms_max\": " << (unique_frames > 1 ? expected_step_max : 0.0) << ",\n"
        << "    \"backward_steps\": " << backward_steps << ",\n"
        << "    \"generated_frames_observed\": " << generated_frames << ",\n"
        << "    \"lanes\": {\n";
    for (std::size_t lane = 0; lane < std::size(osss::kTestPatternLanes); ++lane) {
        const LaneTotals& totals = lane_totals[lane];
        const double infinity = std::numeric_limits<double>::infinity();
        output << "      \"" << osss::kTestPatternLanes[lane].name << "\": {";
        output << "\"generated_psnr_db\": ";
        WriteJsonNumberOrNull(
            output,
            totals.generated_count
                ? totals.generated_psnr / static_cast<double>(totals.generated_count)
                : infinity);
        output << ", \"generated_bad_percent\": ";
        WriteJsonNumberOrNull(
            output,
            totals.generated_count
                ? totals.generated_bad_percent / static_cast<double>(totals.generated_count)
                : infinity);
        output << ", \"real_psnr_db\": ";
        WriteJsonNumberOrNull(
            output,
            totals.real_count ? totals.real_psnr / static_cast<double>(totals.real_count)
                              : infinity);
        output << ", \"generated_frames\": " << totals.generated_count;
        output << ", \"real_frames\": " << totals.real_count << "}";
        output << (lane + 1 < std::size(osss::kTestPatternLanes) ? ",\n" : "\n");
    }
    output
        << "    },\n"
        << "    \"real_frames_observed\": " << real_frames << ",\n"
        << "    \"real_frame_phase_tolerance_ms\": " << kRealFramePhaseToleranceMilliseconds << ",\n"
        << "    \"verdict\": \"" << verdict << "\",\n"
        << "    \"occluded_frames\": " << occluded_frames << ",\n"
        << "    \"occluded_by\": \"" << JsonEscape(NarrowAscii(blocker)) << "\",\n"
        << "    \"worst_channel_error\": " << worst_channel_error << ",\n"
        << "    \"scoring_ms\": "
        << std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - scoring_started).count() << "\n"
        << "  },\n"
        << "  \"frames\": [\n";
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        const auto& frame = frames[index];
        output
            << "    {\"index\": " << index
            << ", \"capture_animation_seconds\": " << frame.animation_seconds
            << ", \"capture_cost_ms\": " << frame.capture_milliseconds
            << ", \"identical_to_previous\": " << (row.identical_to_previous ? "true" : "false")
            << ", \"occluded\": " << (frame.occlusion.occluded ? "true" : "false")
            << ", \"occluded_by\": \"" << JsonEscape(NarrowAscii(frame.occlusion.blocker)) << "\""
            << ", \"best_expected_seconds\": " << row.match.expected_seconds
            << ", \"temporal_offset_ms\": " << row.match.temporal_offset_milliseconds
            << ", \"source_phase_ms\": " << row.source_phase_milliseconds
            << ", \"classification\": \""
            << (row.identical_to_previous ? "repeat" : (row.generated ? "generated" : "real")) << "\""
            << ", \"mae\": " << row.match.metrics.mean_absolute_error
            << ", \"rmse\": " << row.match.metrics.root_mean_square_error
            << ", \"psnr_db\": ";
        WriteJsonNumberOrNull(output, row.match.metrics.psnr_db);
        output
            << ", \"pixels_over_8_percent\": " << row.match.metrics.pixels_over_threshold_percent
            << ", \"maximum_channel_error\": " << static_cast<unsigned>(row.match.metrics.maximum_channel_error)
            << ", \"observed_ppm\": \"" << JsonEscape(row.observed_path.string()) << "\""
            << ", \"expected_ppm\": \"" << JsonEscape(row.expected_path.string()) << "\"}"
            << (index + 1 < rows.size() ? ",\n" : "\n");
    }
    output
        << "  ],\n"
        << "  \"interpretation\": \"Read verdict first. 'occluded' means a foreign window covered the capture rectangle, in which case every other figure describes that window and none of them look wrong: an unrelated image matches the temporal search at an arbitrary instant, so its frames classify as generated and the run resembles a good one. Otherwise: frames are captured while the source keeps presenting. identical_to_previous means the desktop had not changed between captures; unique_frames versus captured_frames therefore bounds the observable output rate. expected_step_ms tracks the ground-truth time between unique frames; negative steps (backward_steps) mean a later capture showed an earlier moment. source_phase_ms is the matched instant's distance from the nearest real source frame instant: a real frame (captured, held, or re-presented) sits within real_frame_phase_tolerance_ms of zero, a generated frame does not, and a session that never generated a frame scores every unique frame as real however good its PSNR looks. Pixel metrics are best-aligned image variance and do not prove monitor presentation.\"\n"
        << "}\n";

    std::cout << std::fixed << std::setprecision(2)
        << "burst " << burst_index << ": " << rows.size() << " frames ("
        << unique_frames << " unique, " << generated_frames << " generated, " << real_frames
        << " real) over " << capture_span_ms << " ms; psnr min/mean/max ";
    if (psnr_count) {
        std::cout << psnr_min << '/' << psnr_sum / static_cast<double>(psnr_count) << '/' << psnr_max;
    } else {
        std::cout << "exact";
    }
    std::cout
        << " dB; offset " << offset_min << ".." << offset_max << " ms; backward steps "
        << backward_steps << "; verdict " << verdict << "; report " << report_path.string() << '\n';
    if (occluded_frames > 0) {
        // Loud, and above every other number, because the numbers on the line
        // above are the ones that mislead.
        std::cout << "  WARNING: " << occluded_frames << " of " << rows.size()
                  << " captures were covered by " << NarrowAscii(blocker)
                  << ". Every figure in this burst describes that window, not the source."
                     " Uncover the target and run it again.\n";
    }

    std::wostringstream title;
    title << BaseTitle(options) << L" | ";
    if (occluded_frames > 0) {
        title << L"OCCLUDED by " << blocker << L" -- burst invalid";
        return title.str();
    }
    title << (output_visible ? L"OSSS" : L"native")
          << L" burst: " << unique_frames << L'/' << rows.size() << L" unique, "
          << generated_frames << L" generated, ";
    if (psnr_count) {
        title << std::fixed << std::setprecision(1) << psnr_min << L'-' << psnr_max << L" dB";
    } else {
        title << L"exact";
    }
    return title.str();
}

std::int64_t QueryCounter() {
    LARGE_INTEGER value{};
    if (!QueryPerformanceCounter(&value)) {
        throw std::runtime_error("QueryPerformanceCounter failed.");
    }
    return value.QuadPart;
}

double SecondsBetween(
    const std::int64_t later,
    const std::int64_t earlier,
    const std::int64_t frequency) noexcept {
    return static_cast<double>(later - earlier) / static_cast<double>(frequency);
}

void ArmRelativeTimer(const HANDLE timer, const double seconds) {
    LARGE_INTEGER due{};
    due.QuadPart = -std::max<std::int64_t>(
        1,
        static_cast<std::int64_t>(std::llround(seconds * 10'000'000.0)));
    if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        throw std::runtime_error("SetWaitableTimer failed.");
    }
}

int RunInteractive(const Options& options) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!RegisterWindowClass(instance)) {
        throw std::runtime_error("Could not register the test-animation window class.");
    }
    WindowState state;
    const osss::TestPatternSpec specification{
        osss::TestPatternSpec::kDefaultWidth,
        osss::TestPatternSpec::kDefaultHeight,
        options.api,
        options.base_fps,
        options.hud_overlay,
    };
    if (options.hud_overlay) {
        std::wcout
            << L"HUD overlay on. Matching OSSS argument:\n  --ui-mask \""
            << std::filesystem::path(osss::TestPatternHudMaskArgument(specification)).wstring()
            << L"\"\n";
    }
    const HWND window = CreateAnimationWindow(
        instance,
        state,
        options,
        specification.width,
        specification.height);
    if (!window) {
        throw std::runtime_error("Could not create the test-animation window.");
    }
    auto backend = osss::CreateTestAnimationBackend(
        options.api,
        window,
        specification.width,
        specification.height);
    SetWindowTextW(window, BaseTitle(options).c_str());
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);

    LARGE_INTEGER frequency_value{};
    if (!QueryPerformanceFrequency(&frequency_value) || frequency_value.QuadPart <= 0) {
        throw std::runtime_error("QueryPerformanceFrequency failed.");
    }
    const std::int64_t frequency = frequency_value.QuadPart;
    std::int64_t epoch = QueryCounter();
    std::int64_t pause_started = 0;
    std::uint64_t last_frame = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t snapshot_index = 0;
    std::uint64_t burst_index = 0;
    bool paused = false;
    const auto session_directory = CreateSessionDirectory(options);
    WriteSessionManifest(session_directory, options, frequency);

    // Burst capture state. Captures are interleaved with source presentation so
    // the source cadence OSSS observes is not disturbed by the capture itself.
    const std::uint32_t burst_frames = options.burst_frames == 0 ? 10U : options.burst_frames;
    const double burst_interval_seconds = options.burst_interval_milliseconds > 0.0
        ? options.burst_interval_milliseconds / 1000.0
        : kDefaultBurstIntervalSourcePeriods / static_cast<double>(options.base_fps);
    bool burst_active = false;
    bool cli_burst_pending = options.burst_at_milliseconds >= 0.0;
    std::int64_t next_capture_counter = 0;
    std::vector<BurstFrame> burst;
    std::optional<ScopedLauncherDisplacement> burst_displacement;
    bool burst_output_visible = false;

    HANDLE raw_timer = CreateWaitableTimerExW(
        nullptr,
        nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    if (!raw_timer) {
        raw_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    if (!raw_timer) {
        throw std::runtime_error("Could not create the test-animation pacing timer.");
    }
    const ScopedHandle timer(raw_timer);

    while (state.running) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                state.running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!state.running) {
            break;
        }

        const std::int64_t now = QueryCounter();
        if (state.reset_requested) {
            epoch = now;
            if (paused) {
                pause_started = now;
            }
            last_frame = std::numeric_limits<std::uint64_t>::max();
            state.reset_requested = false;
            SetWindowTextW(window, BaseTitle(options).c_str());
        }
        if (state.toggle_pause_requested) {
            if (paused) {
                epoch += now - pause_started;
                paused = false;
            } else {
                pause_started = now;
                paused = true;
            }
            state.toggle_pause_requested = false;
        }

        const std::int64_t animation_now = paused ? pause_started : now;
        const double animation_seconds = SecondsBetween(animation_now, epoch, frequency);
        if (state.snapshot_requested) {
            SaveAndScoreSnapshot(
                window,
                options,
                specification,
                session_directory,
                ++snapshot_index,
                std::max(0.0, animation_seconds));
            state.snapshot_requested = false;
        }
        if (cli_burst_pending && !paused &&
            animation_seconds * 1000.0 >= options.burst_at_milliseconds) {
            state.burst_requested = true;
            cli_burst_pending = false;
        }
        if (state.burst_requested) {
            state.burst_requested = false;
            if (!burst_active && !paused) {
                burst_active = true;
                burst.clear();
                burst.reserve(burst_frames);
                burst_displacement.emplace(window, specification.width, specification.height);
                // The first BitBlt of a session pays GDI setup (~15 ms here) and
                // returns a staler frame than the rest, which would show up as a
                // false latency spike on frame 0. Warm the path and discard it.
                CaptureClientPixels(
                    window,
                    specification.width,
                    specification.height,
                    options.capture_origin);
                next_capture_counter = QueryCounter();
            }
        }
        if (burst_active && QueryCounter() >= next_capture_counter) {
            BurstFrame frame;
            const std::int64_t capture_started = QueryCounter();
            frame.animation_seconds = std::max(0.0, SecondsBetween(capture_started, epoch, frequency));
            frame.observed = CaptureClientPixels(
                window,
                specification.width,
                specification.height,
                options.capture_origin,
                &frame.occlusion);
            const std::int64_t capture_finished = QueryCounter();
            frame.capture_milliseconds = SecondsBetween(capture_finished, capture_started, frequency) * 1000.0;
            burst.push_back(std::move(frame));
            // Schedule from the intended time, not the actual one, so capture cost
            // does not accumulate as drift across the burst.
            next_capture_counter += static_cast<std::int64_t>(
                std::llround(burst_interval_seconds * static_cast<double>(frequency)));
            if (next_capture_counter < capture_finished) {
                next_capture_counter = capture_finished;
            }
            if (burst.size() >= burst_frames) {
                burst_output_visible = FrameOutputIsVisibleOver(window);
                burst_displacement.reset();
                burst_active = false;
                const std::wstring title = ScoreAndWriteBurst(
                    options,
                    specification,
                    session_directory,
                    ++burst_index,
                    burst,
                    burst_frames,
                    burst_interval_seconds * 1000.0,
                    burst_output_visible);
                burst.clear();
                SetWindowTextW(window, title.c_str());
                if (options.exit_after_burst) {
                    DestroyWindow(window);
                    continue;
                }
            }
        }
        if (state.open_folder_requested) {
            ShellExecuteW(
                window,
                L"open",
                session_directory.c_str(),
                nullptr,
                nullptr,
                SW_SHOWNORMAL);
            state.open_folder_requested = false;
        }

        if (!paused && animation_seconds >= 0.0) {
            const auto due_frame = static_cast<std::uint64_t>(
                std::floor(animation_seconds * static_cast<double>(options.base_fps) + 1.0e-9));
            if (due_frame != last_frame) {
                const double scheduled_seconds =
                    static_cast<double>(due_frame) / static_cast<double>(options.base_fps);
                const auto pixels = osss::RenderTestPattern(
                    specification,
                    scheduled_seconds,
                    due_frame);
                backend->Present(pixels);
                last_frame = due_frame;
            }
        }
        if (options.duration_seconds > 0.0 && animation_seconds >= options.duration_seconds) {
            DestroyWindow(window);
            continue;
        }

        if (paused) {
            MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }
        const double next_frame_seconds =
            static_cast<double>(last_frame + 1) / static_cast<double>(options.base_fps);
        const std::int64_t wait_now = QueryCounter();
        double remaining = next_frame_seconds - SecondsBetween(wait_now, epoch, frequency);
        if (burst_active) {
            remaining = std::min(remaining, SecondsBetween(next_capture_counter, wait_now, frequency));
        }
        if (remaining > 0.0004) {
            ArmRelativeTimer(timer.Get(), remaining - 0.0002);
            const HANDLE handles[]{timer.Get()};
            MsgWaitForMultipleObjectsEx(1, handles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } else {
            SwitchToThread();
        }
    }
    backend.reset();
    UnregisterClassW(kWindowClassName, instance);
    return 0;
}

int ExportReference(const Options& options) {
    const osss::TestPatternSpec specification{
        osss::TestPatternSpec::kDefaultWidth,
        osss::TestPatternSpec::kDefaultHeight,
        options.api,
        options.base_fps,
        options.hud_overlay,
    };
    const double seconds = options.time_milliseconds / 1000.0;
    const auto pixels = osss::RenderTestPattern(
        specification,
        seconds,
        static_cast<std::uint64_t>(std::floor(seconds * options.base_fps)));
    std::string error;
    if (!osss::WriteTestPatternPpm(
            *options.export_reference,
            pixels,
            specification.width,
            specification.height,
            error)) {
        throw std::runtime_error(error);
    }
    std::wcout << L"Wrote deterministic reference: " << options.export_reference->c_str() << L'\n';
    return 0;
}

int CompareImage(const Options& options) {
    std::vector<std::uint32_t> observed;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string error;
    if (!osss::ReadTestPatternPpm(*options.compare_image, observed, width, height, error)) {
        throw std::runtime_error(error);
    }
    const osss::TestPatternSpec specification{
        width,
        height,
        options.api,
        options.base_fps,
        options.hud_overlay,
    };
    const MatchResult match = FindBestMatch(
        specification,
        observed,
        options.time_milliseconds / 1000.0,
        options.search_milliseconds);
    std::cout << std::fixed << std::setprecision(6)
        << "{\"best_expected_seconds\":" << match.expected_seconds
        << ",\"temporal_offset_ms\":" << match.temporal_offset_milliseconds
        << ",\"mae\":" << match.metrics.mean_absolute_error
        << ",\"rmse\":" << match.metrics.root_mean_square_error
        << ",\"psnr_db\":";
    if (std::isinf(match.metrics.psnr_db)) {
        std::cout << "null";
    } else {
        std::cout << match.metrics.psnr_db;
    }
    std::cout << ",\"pixels_over_8_percent\":"
        << match.metrics.pixels_over_threshold_percent
        << ",\"maximum_channel_error\":"
        << static_cast<unsigned>(match.metrics.maximum_channel_error);
    if (match.hud_overlay_checked) {
        std::cout
            << ",\"hud_overlay\":{\"excluded_from_pixel_metrics\":true"
            << ",\"holds_one_real_source_frame\":"
            << (match.hud.matches_discrete_state ? "true" : "false")
            << ",\"matched_source_frame_index\":" << match.hud.matched_source_frame_index
            << ",\"maximum_channel_error_to_nearest_state\":"
            << static_cast<unsigned>(match.hud.maximum_channel_error) << "}";
    }
    std::cout << "}\n";
    return 0;
}

int RunSelfTest() {
    osss::TestPatternSpec specification;
    const auto first = osss::RenderTestPattern(specification, 1.25, 75);
    const auto repeated = osss::RenderTestPattern(specification, 1.25, 75);
    const auto cut = osss::RenderTestPattern(specification, 3.25, 195);
    const auto exact = osss::CompareTestPatternFrames(
        first,
        repeated,
        specification.width,
        specification.height);
    const auto changed = osss::CompareTestPatternFrames(
        first,
        cut,
        specification.width,
        specification.height);
    if (exact.root_mean_square_error != 0.0 || !std::isinf(exact.psnr_db) ||
        changed.root_mean_square_error < 20.0) {
        throw std::runtime_error("Deterministic test-pattern self-test failed.");
    }
    std::cout << "OSSS deterministic test-pattern self-test passed.\n";
    return 0;
}

} // namespace

int wmain(const int argc, wchar_t** argv) {
    try {
        if (!osss::EnablePerMonitorV2DpiAwareness()) {
            throw std::runtime_error("Could not enable per-monitor V2 DPI awareness.");
        }
        const Options options = ParseOptions(argc, argv);
        if (options.help) {
            PrintUsage();
            return 0;
        }
        if (options.self_test) {
            return RunSelfTest();
        }
        if (options.export_reference) {
            return ExportReference(options);
        }
        if (options.compare_image) {
            return CompareImage(options);
        }
        return RunInteractive(options);
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
