#include "stats_overlay.h"

#include "window_catalog.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

namespace osss {
namespace {

std::wstring FrameRateText(const double frames_per_second, const bool has_sample) {
    if (!has_sample) {
        return L"--.- FPS";
    }
    std::wostringstream text;
    text << std::fixed << std::setprecision(1) << std::max(0.0, frames_per_second) << L" FPS";
    return text.str();
}

std::wstring OptionalFrameRateText(
    const std::optional<double> frames_per_second,
    const bool has_sample) {
    return frames_per_second
        ? FrameRateText(*frames_per_second, has_sample)
        : L"n/a";
}

// Native is printed as 100 minus the rounded generated percent rather than as
// its own rounding of (1 - share), so the two halves always read as 100.
std::wstring GeneratedShareText(const double generated_share, const bool has_sample) {
    if (!has_sample) {
        return L"FRAMES  generated --%   native --%";
    }
    const long generated_percent =
        std::lround(std::clamp(generated_share, 0.0, 1.0) * 100.0);
    std::wostringstream text;
    text << L"FRAMES  generated " << generated_percent << L"%"
        << L"   native " << (100 - generated_percent) << L"%";
    return text.str();
}

void DrawTextLine(
    const HDC device_context,
    const std::wstring& text,
    RECT bounds,
    const UINT format) {
    DrawTextW(
        device_context,
        text.c_str(),
        static_cast<int>(text.size()),
        &bounds,
        format);
}

} // namespace

StatsOverlay::~StatsOverlay() {
    Release();
}

bool StatsOverlay::Create(
    const RECT& target_bounds,
    const int max_multiplier,
    const double target_fps,
    const bool motion_enabled) {
    Release();
    max_multiplier_ = std::clamp(max_multiplier, 2, 6);
    target_fps_ = std::max(0.0, target_fps);
    motion_enabled_ = motion_enabled;
    target_bounds_ = target_bounds;

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WindowProcedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClassName;
    if (RegisterClassExW(&window_class) == 0) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
    } else {
        window_class_registered_ = true;
    }

    window_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
        kWindowClassName,
        L"OSSS Performance",
        WS_POPUP,
        target_bounds.left + kOffsetDip,
        target_bounds.top + kOffsetDip,
        kWidthDip,
        kHeightDip,
        nullptr,
        nullptr,
        instance,
        this);
    if (!window_) {
        Release();
        return false;
    }

    if (!UpdateScale(GetDpiForWindow(window_), target_bounds_) ||
        !SetLayeredWindowAttributes(window_, 0, 232, LWA_ALPHA)) {
        Release();
        return false;
    }

    Position(target_bounds_, false);
    ApplyRoundedRegion(false);
    return true;
}

int StatsOverlay::ScaleDip(const int value) const noexcept {
    return MulDiv(value, static_cast<int>(layout_dpi_), static_cast<int>(kDefaultDpi));
}

bool StatsOverlay::UpdateScale(UINT monitor_dpi, const RECT& target_bounds) {
    if (monitor_dpi == 0) {
        monitor_dpi = kDefaultDpi;
    }

    const int target_width = std::max(
        1,
        static_cast<int>(target_bounds.right - target_bounds.left));
    const int target_height = std::max(
        1,
        static_cast<int>(target_bounds.bottom - target_bounds.top));
    const int width_fit_dpi = static_cast<int>(
        static_cast<std::int64_t>(target_width) * kDefaultDpi /
        (kWidthDip + 2 * kOffsetDip));
    const int height_fit_dpi = static_cast<int>(
        static_cast<std::int64_t>(target_height) * kDefaultDpi /
        (kHeightDip + 2 * kOffsetDip));
    const UINT target_fit_dpi = static_cast<UINT>(
        std::max(1, std::min(width_fit_dpi, height_fit_dpi)));
    const UINT layout_dpi = std::max(
        kMinimumLayoutDpi,
        std::min(monitor_dpi, target_fit_dpi));

    if (layout_dpi == layout_dpi_ && label_font_ && value_font_) {
        return true;
    }

    const HFONT label_font = CreateFontW(
        -MulDiv(13, static_cast<int>(layout_dpi), static_cast<int>(kDefaultDpi)),
        0,
        0,
        0,
        FW_SEMIBOLD,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
    const HFONT value_font = CreateFontW(
        -MulDiv(21, static_cast<int>(layout_dpi), static_cast<int>(kDefaultDpi)),
        0,
        0,
        0,
        FW_SEMIBOLD,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
    if (!label_font || !value_font) {
        if (label_font) {
            DeleteObject(label_font);
        }
        if (value_font) {
            DeleteObject(value_font);
        }
        return false;
    }

    if (label_font_) {
        DeleteObject(label_font_);
    }
    if (value_font_) {
        DeleteObject(value_font_);
    }
    label_font_ = label_font;
    value_font_ = value_font;
    layout_dpi_ = layout_dpi;
    return true;
}

void StatsOverlay::ApplyRoundedRegion(const bool redraw) {
    if (!window_) {
        return;
    }
    const HRGN rounded_region = CreateRoundRectRgn(
        0,
        0,
        ScaleDip(kWidthDip) + 1,
        ScaleDip(kHeightDip) + 1,
        ScaleDip(14),
        ScaleDip(14));
    if (rounded_region && SetWindowRgn(window_, rounded_region, redraw ? TRUE : FALSE) == 0) {
        DeleteObject(rounded_region);
    }
}

void StatsOverlay::HandleDpiChanged(const UINT dpi, const RECT& suggested_bounds) {
    if (!window_ || !UpdateScale(dpi, target_bounds_)) {
        return;
    }
    SetWindowPos(
        window_,
        nullptr,
        suggested_bounds.left,
        suggested_bounds.top,
        ScaleDip(kWidthDip),
        ScaleDip(kHeightDip),
        SWP_NOACTIVATE | SWP_NOZORDER);
    ApplyRoundedRegion(true);
    InvalidateRect(window_, nullptr, FALSE);
}

void StatsOverlay::Show() {
    if (!window_ || visible_) {
        return;
    }
    visible_ = true;
    Position(target_bounds_, true);
    InvalidateRect(window_, nullptr, FALSE);
}

void StatsOverlay::Update(const double source_fps, const double output_fps) {
    RuntimeStats statistics{};
    statistics.raw_capture_fps = source_fps;
    statistics.unique_source_fps = source_fps;
    statistics.target_fps = target_fps_;
    statistics.submitted_fps = output_fps;
    Update(statistics);
}

void StatsOverlay::Update(const RuntimeStats& statistics) {
    statistics_ = statistics;
    statistics_.raw_capture_fps = std::max(0.0, statistics_.raw_capture_fps);
    statistics_.unique_source_fps = std::max(0.0, statistics_.unique_source_fps);
    statistics_.target_fps = std::max(0.0, statistics_.target_fps);
    statistics_.submitted_fps = std::max(0.0, statistics_.submitted_fps);
    if (statistics_.confirmed_fps) {
        statistics_.confirmed_fps = std::max(0.0, *statistics_.confirmed_fps);
    }
    statistics_.generated_share = std::clamp(statistics_.generated_share, 0.0, 1.0);
    statistics_.pacing_on_time_fraction =
        std::clamp(statistics_.pacing_on_time_fraction, 0.0, 1.0);
    has_sample_ = true;
    if (window_) {
        InvalidateRect(window_, nullptr, FALSE);
        UpdateWindow(window_);
    }
}

void StatsOverlay::FollowTarget(const HWND target) {
    if (const auto bounds = ExtendedWindowBounds(target)) {
        const UINT previous_layout_dpi = layout_dpi_;
        const bool scale_updated = UpdateScale(GetDpiForWindow(target), *bounds);
        Position(*bounds, visible_);
        if (scale_updated && layout_dpi_ != previous_layout_dpi) {
            ApplyRoundedRegion(true);
            InvalidateRect(window_, nullptr, FALSE);
        }
    }
}

HWND StatsOverlay::Window() const noexcept {
    return window_;
}

LRESULT CALLBACK StatsOverlay::WindowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
    auto* overlay = reinterpret_cast<StatsOverlay*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        overlay = static_cast<StatsOverlay*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(overlay));
    }

    switch (message) {
    case WM_PAINT:
        if (overlay) {
            overlay->Paint();
            return 0;
        }
        break;
    case WM_DPICHANGED:
        if (overlay) {
            const auto* suggested_bounds = reinterpret_cast<const RECT*>(lparam);
            if (suggested_bounds) {
                overlay->HandleDpiChanged(HIWORD(wparam), *suggested_bounds);
            }
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void StatsOverlay::Paint() {
    if (!window_) {
        return;
    }

    PAINTSTRUCT paint{};
    const HDC device_context = BeginPaint(window_, &paint);
    RECT client{};
    GetClientRect(window_, &client);
    const HBRUSH background = CreateSolidBrush(RGB(13, 17, 23));
    FillRect(device_context, &client, background);
    DeleteObject(background);

    SetBkMode(device_context, TRANSPARENT);
    const auto bounds = [this](const int left, const int top, const int right, const int bottom) {
        return RECT{ScaleDip(left), ScaleDip(top), ScaleDip(right), ScaleDip(bottom)};
    };

    SetTextColor(device_context, RGB(111, 232, 174));
    SelectObject(device_context, label_font_);
    std::wostringstream header;
    header << L"OSSS   " << std::fixed << std::setprecision(3) << target_fps_
        << L" FPS TARGET   " << max_multiplier_ << L"x MAX   "
        << (!statistics_.generation_enabled
                ? L"GEN OFF"
                : motion_enabled_ ? L"MOTION" : L"BLEND");
    DrawTextLine(device_context, header.str(), bounds(12, 7, kWidthDip - 12, 26),
        DT_LEFT | DT_SINGLELINE);

    SetTextColor(device_context, RGB(154, 164, 178));
    DrawTextLine(device_context, L"RAW", bounds(12, 31, 105, 49), DT_LEFT | DT_SINGLELINE);
    DrawTextLine(device_context, L"UNIQUE", bounds(126, 31, 235, 49), DT_LEFT | DT_SINGLELINE);
    DrawTextLine(device_context, L"SUBMITTED", bounds(252, 31, 375, 49), DT_LEFT | DT_SINGLELINE);
    DrawTextLine(device_context, L"DISPLAY", bounds(390, 31, kWidthDip - 12, 49),
        DT_LEFT | DT_SINGLELINE);

    SelectObject(device_context, value_font_);
    SetTextColor(device_context, RGB(244, 247, 251));
    DrawTextLine(device_context, FrameRateText(statistics_.raw_capture_fps, has_sample_),
        bounds(12, 48, 122, 76), DT_LEFT | DT_SINGLELINE);
    DrawTextLine(device_context, FrameRateText(statistics_.unique_source_fps, has_sample_),
        bounds(126, 48, 248, 76), DT_LEFT | DT_SINGLELINE);
    DrawTextLine(device_context, FrameRateText(statistics_.submitted_fps, has_sample_),
        bounds(252, 48, 386, 76), DT_LEFT | DT_SINGLELINE);
    DrawTextLine(device_context, OptionalFrameRateText(statistics_.confirmed_fps, has_sample_),
        bounds(390, 48, kWidthDip - 10, 76), DT_LEFT | DT_SINGLELINE);

    SelectObject(device_context, label_font_);
    SetTextColor(device_context, RGB(186, 196, 211));
    std::wostringstream multiplier_line;
    multiplier_line << std::fixed << std::setprecision(2)
        << L"MULTIPLIER  required " << statistics_.required_multiplier << L"x"
        << L"   allowed " << statistics_.allowed_multiplier << L"x"
        << L"   realized " << statistics_.realized_multiplier << L"x";
    DrawTextLine(device_context, multiplier_line.str(), bounds(12, 82, kWidthDip - 12, 103),
        DT_LEFT | DT_SINGLELINE);

    DrawTextLine(device_context, GeneratedShareText(statistics_.generated_share, has_sample_),
        bounds(12, 106, kWidthDip - 12, 127), DT_LEFT | DT_SINGLELINE);

    std::wostringstream queue_line;
    queue_line << std::fixed << std::setprecision(1)
        << L"QUEUE  " << statistics_.queue_occupancy
        << L" frames   delay " << statistics_.queue_delay_milliseconds << L" ms"
        << L"   capture-to-present " << statistics_.capture_to_present_milliseconds << L" ms";
    DrawTextLine(device_context, queue_line.str(), bounds(12, 130, kWidthDip - 12, 151),
        DT_LEFT | DT_SINGLELINE);

    std::wostringstream state_line;
    state_line << L"MISSED  ";
    if (statistics_.pacing_clock_owned) {
        state_line << statistics_.missed_deadlines;
    } else {
        state_line << L"--";
    }
    state_line << L"     DUPLICATES  " << statistics_.duplicate_frames
        << L"     CAPTURE DROPS  " << statistics_.dropped_frames;
    DrawTextLine(device_context, state_line.str(), bounds(12, 154, kWidthDip - 12, 175),
        DT_LEFT | DT_SINGLELINE);

    // Frame times, not frame counts. Every line above this one can look correct
    // while the output judders; this is the line that shows it. Under a
    // free-running loop the intervals are still real -- they are simply
    // present-to-present rather than deadline-to-handover -- but there is no
    // target period to be on time against, so that column is not printed.
    std::wostringstream pacing_line;
    if (statistics_.pacing_sample_count == 0) {
        pacing_line << L"PACING  --";
    } else {
        pacing_line << std::fixed << std::setprecision(2)
            << (statistics_.pacing_clock_owned ? L"PACING  p50 " : L"PACING (free-run)  p50 ")
            << statistics_.pacing_p50_milliseconds << L" ms"
            << L"   p95 " << statistics_.pacing_p95_milliseconds << L" ms"
            << L"   max " << statistics_.pacing_maximum_milliseconds << L" ms";
        if (statistics_.pacing_clock_owned) {
            pacing_line << L"   on-time " << std::setprecision(0)
                << statistics_.pacing_on_time_fraction * 100.0 << L"%";
        }
    }
    DrawTextLine(device_context, pacing_line.str(), bounds(12, 178, kWidthDip - 12, 199),
        DT_LEFT | DT_SINGLELINE);

    EndPaint(window_, &paint);
}

void StatsOverlay::Hide() {
    if (!window_ || !visible_) {
        return;
    }
    ShowWindow(window_, SW_HIDE);
    visible_ = false;
}

void StatsOverlay::Position(const RECT& target_bounds, const bool show) {
    if (!window_) {
        return;
    }
    target_bounds_ = target_bounds;
    SetWindowPos(
        window_,
        HWND_TOPMOST,
        target_bounds.left + ScaleDip(kOffsetDip),
        target_bounds.top + ScaleDip(kOffsetDip),
        ScaleDip(kWidthDip),
        ScaleDip(kHeightDip),
        SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : SWP_NOREDRAW));
}

void StatsOverlay::Release() noexcept {
    if (window_) {
        if (IsWindow(window_)) {
            DestroyWindow(window_);
        }
        window_ = nullptr;
    }
    if (label_font_) {
        DeleteObject(label_font_);
        label_font_ = nullptr;
    }
    if (value_font_) {
        DeleteObject(value_font_);
        value_font_ = nullptr;
    }
    if (window_class_registered_) {
        UnregisterClassW(kWindowClassName, GetModuleHandleW(nullptr));
        window_class_registered_ = false;
    }
    visible_ = false;
    has_sample_ = false;
    layout_dpi_ = kDefaultDpi;
}

} // namespace osss
