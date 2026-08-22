#include "adaptive_scheduler.h"
#include "app_profile.h"
#include "debug_view.h"
#include "dpi_awareness.h"
#include "flow_scale.h"
#include "frame_rate_limits.h"
#include "launcher_theme.h"
#include "output_mode.h"
#include "pacing_mode.h"
#include "present_mode.h"
#include "test_animation_catalog.h"
#include "ui_mask.h"
#include "upscaler.h"
#include "window_catalog.h"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>

// Present in newer SDK headers only; the value is stable and documented.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using osss::LauncherLayout;
using osss::LauncherPalette;
using osss::RectHeight;
using osss::RectWidth;
using osss::ShadeColor;

constexpr wchar_t kLauncherClassName[] = L"OSSS.SettingsLauncher";
constexpr wchar_t kSettingsHostClassName[] = L"OSSS.SettingsHost";
constexpr wchar_t kLauncherTitle[] = L"OSSS Frame Generation";
constexpr UINT_PTR kProcessTimerId = 1;
constexpr std::array<int, 10> kTargetFpsOptions{60, 75, 90, 120, 144, 165, 180, 240, 360, 480};
constexpr std::array<int, 7> kBufferFloorOptions{0, 4, 8, 12, 16, 24, 32};
constexpr int kTestAnimationMenuFirst = 4000;
constexpr int kTestAnimationMenuCount = static_cast<int>(
    osss::kTestGraphicsApis.size() * osss::kTestAnimationBaseRates.size());
constexpr int kTestAnimationMenuLast = kTestAnimationMenuFirst + kTestAnimationMenuCount - 1;

// Window geometry, in the logical pixels LauncherLayout deals in. The two
// client heights are not constants: they fall out of the layout walk that
// creates the controls, clamped to the work area by ApplyGeometry.
constexpr int kWindowWidth = 720;
constexpr int kHostTop = 80;
constexpr int kStatusGap = 22;
constexpr int kStatusHeight = 62;
constexpr int kFooterGap = 20;
constexpr int kFooterHeight = 64;
constexpr int kBelowHost = kStatusGap + kStatusHeight + kFooterGap + kFooterHeight;
// The disclosure row, and the gap that separates it from the section above and
// from the first advanced caption below it.
constexpr int kDisclosureHeight = 38;
constexpr int kDisclosureGap = 16;
constexpr int kAdvancedTop = 12;
// Reserved below the work area so the window never fills the screen edge to
// edge; the design's clamp.
constexpr int kWorkAreaReserve = 96;

enum ControlId : int {
    kTargetCombo = 1001,
    kRefreshButton,
    kTargetFpsCombo,
    kMultiplierCombo,
    kBufferCombo,
    kCeilingPacingCombo,
    kPresentModeCombo,
    kPacingModeCombo,
    kFlowScaleCombo,
    kPerformanceModeCheckbox,
    kTemporalPriorCheckbox,
    kOutputModeCombo,
    kUpscaleCombo,
    kDebugViewCombo,
    kProfileCheckbox,
    kProfileSaveButton,
    kInterpolatorCombo,
    kUiMaskEdit,
    kUiMaskAutoCheckbox,
    kStatsCheckbox,
    kStartButton,
    kStopButton,
    kStatusText,
    // New with the redesign. Appended rather than inserted so every existing
    // WM_COMMAND route keeps its value.
    kAdvancedToggle,
};

// The status panel's dot colour, and the only thing in the launcher that
// carries meaning through colour alone -- which is why the headline beside it
// always says the same thing in words.
enum class StatusLevel { ok, warning, failure };

// Which palette entry a STATIC's text is drawn in. Stored as a role rather than
// a resolved COLORREF so a theme switch does not have to walk back over every
// control it coloured at creation.
enum class TextRole : LONG_PTR {
    none = 0,
    normal,
    muted,
    faint,
    warning,
};

// How a subclassed control paints itself. Every interactive control in the
// launcher is painted here rather than by the system, for one reason: the user
// runs the app theme the OS tells us to run, and a COMBOBOX cannot be made dark
// through WM_CTLCOLOR* alone. Painting all of them the same way is also what
// keeps a dark window from ending up full of light drop-downs, which the design
// calls out as worse than shipping light-only.
enum class SkinKind {
    push_button,
    accent_button,
    checkbox,
    combo,
    disclosure,
};

struct LauncherState;

struct ControlSkin {
    SkinKind kind = SkinKind::push_button;
    LauncherState* state = nullptr;
    // Whether the control sits on the footer band rather than the window
    // background. The rounded corners let the parent through, so the corner
    // pixels have to be painted in whichever of the two it actually is -- and
    // it is stored as which, not as a colour, so a theme switch resolves it
    // again instead of leaving a stale fill behind every button.
    bool on_band = false;
    bool hot = false;
};

// One tooltip, addressed by the control it hangs off. The body is supplied
// through TTN_GETDISPINFO rather than at TTM_ADDTOOL time so the title can be
// set for the tool that is about to show: TTM_SETTITLE applies to the tooltip
// window, not to a tool, so it has to be re-sent per display.
enum class Tip : std::size_t {
    target_window,
    refresh,
    output_target,
    maximum_interpolation,
    interpolator,
    stats_overlay,
    present_mode,
    pacing_mode,
    buffer_floor,
    ceiling_pacing,
    flow_scale,
    upscale,
    performance_mode,
    temporal_prior,
    output_shape,
    ui_mask,
    ui_mask_auto,
    debug_view,
    profile,
    start,
    count,
};

struct TipText {
    const wchar_t* title;
    const wchar_t* body;
};

// Every claim below traces to README.md. Keep it that way: a tooltip that
// overstates what is verified is worse than no tooltip, because it is read at
// the moment the user is deciding.
constexpr std::array<TipText, static_cast<std::size_t>(Tip::count)> kTips{{
    {L"Target window",
     L"The window OSSS captures. Only visible, restored windows are listed - a "
     L"Direct3D exclusive-fullscreen app cannot be captured at all. Refresh "
     L"after opening a new one."},
    {L"Refresh",
     L"Re-enumerates visible, restored windows. Use it when you opened the "
     L"target after this list loaded."},
    {L"Output target",
     L"The rate OSSS paces its output to. Match display keeps the active "
     L"display path's exact numerator and denominator - pick a manual 24-1000 "
     L"FPS rate only when you are measuring something specific."},
    {L"Maximum interpolation",
     L"A ceiling on how many frames one source pair may supply - not a pacing "
     L"target. 6x covers fractional cases like 60→144 (2.4x); go higher "
     L"only for low-rate content such as video or engine-capped titles."},
    {L"Interpolator",
     L"Motion aware estimates optical flow on the GPU once per source pair. "
     L"Temporal blend is the old crossfade, kept as an A/B baseline - pick it "
     L"only when you want that comparison."},
    {L"Show source/output FPS overlay",
     L"A topmost, click-through HUD over the target: source and output rates, "
     L"multipliers, queue depth and pacing. It turns itself off in fullscreen "
     L"output, because anything composed above the output demotes it."},
    {L"Present mode",
     L"How a finished frame reaches the display, and the single largest "
     L"influence on frame pacing. Auto uses tearing wherever DXGI reports "
     L"support for it - pick VSync only if you would rather have judder than "
     L"tearing."},
    // Not in the design, because --pacing landed after it was written. Sourced
    // from README.md's "Pacing modes" table and its measured paragraph.
    {L"Pacing",
     L"When a frame is rendered and handed over relative to the output clock, "
     L"which is independent of present mode. Paced renders each slot at its "
     L"deadline; Queued renders one slot ahead and costs exactly that slot in "
     L"latency; Unpaced has no clock and presents whenever a back buffer is "
     L"free. Measured, all three submitted the same rate - unpaced changes the "
     L"cadence, not the latency."},
    {L"Adaptive buffer floor",
     L"How far behind the compositor OSSS selects known frames. 8 ms by "
     L"default, grows 2 ms after an underrun and caps at 32 - raise it for a "
     L"heavy game that stutters, lower it for latency."},
    {L"Ceiling pacing",
     L"What happens when the multiplier ceiling binds. Even shows each "
     L"admitted frame for a whole number of target slots, which avoids "
     L"alternating frame durations; pick Spread only to compare against the "
     L"older distribution."},
    {L"Flow scale",
     L"The resolution motion estimation runs at, as a divisor of the source "
     L"size. Above 1440p pick Quality: at 4K it passes this repository's "
     L"quality gates that Auto fails, for 4% more GPU time."},
    {L"Upscale",
     L"Sharpens and upsamples the fused frame when the output is larger than "
     L"the capture - the fullscreen case where the target window is smaller "
     L"than the monitor. Leave on Auto unless you are testing the upscaler "
     L"itself."},
    {L"Cheaper motion search",
     L"Halves the coarse search radius and skips the second local search that "
     L"resolves periodic detail. Measured on one GPU: 0.05 ms faster per pair, "
     L"thin detail 7.3 dB worse - leave it off unless your GPU is much weaker."},
    // Also not in the design; --temporal-prior landed with the same batch.
    {L"Temporal prior",
     L"Offers the previous pair's motion field to this pair's coarse search as "
     L"extra candidates, so a pan that accelerates past the search window is "
     L"followed instead of lost. It is a candidate and never a bias - selection "
     L"stays regularised toward zero. On by default; turn it off to A/B the "
     L"estimator."},
    {L"Output",
     L"Overlay is click-through and sized to the target window, and can never "
     L"drive a variable-refresh display - layered windows are always composed. "
     L"Fullscreen is eligible for it, at the cost of passing clicks through."},
    {L"HUD mask regions",
     L"Rectangles that always show the newest real frame, so counters tick "
     L"instead of blending. Write left,top,right,bottom as 0-1 fractions or "
     L"px, separated with ; - use it for minimaps and ammo counters."},
    {L"Detect static HUD regions",
     L"Finds overlays that hold still while the scene around them moves: a "
     L"region arms after about ten source pairs and releases within about "
     L"three. Experimental - explicit rectangles stay the predictable option."},
    {L"Diagnostic view",
     L"Replaces the output with a picture of the interpolator's internals. "
     L"Pick Fallback reason when motion looks smeared: it shows where the "
     L"interpolator gave up and crossfaded."},
    {L"Apply saved profile",
     L"Applies the arguments saved for the target's own executable before "
     L"anything set here, so each program picks up its own settings. Save "
     L"writes the current settings as that program's profile."},
    {L"Start",
     L"Minimizes this launcher and hands the foreground back to the target, "
     L"because the overlay stays hidden while the target does not own the "
     L"foreground. Stop stays reachable from the taskbar."},
}};

struct TooltipEntry {
    HWND control = nullptr;
    Tip tip = Tip::target_window;
};

// Where the layout cursor put a control, in logical pixels, in its parent's
// coordinates. Recorded at creation so RunLauncherSelfTest can assert that no
// two of them intersect -- the check that would have caught the buffer-floor
// combo being painted over, and the reason the cursor exists at all.
struct PlacedControl {
    HWND parent = nullptr;
    HWND control = nullptr;
    RECT rect{};
    bool advanced = false;
};

struct LauncherState {
    HWND window = nullptr;
    HWND settings_host = nullptr;
    HWND tooltip = nullptr;
    HWND target_combo = nullptr;
    HWND refresh_button = nullptr;
    HWND target_fps_combo = nullptr;
    HWND max_multiplier_combo = nullptr;
    HWND buffer_combo = nullptr;
    HWND ceiling_pacing_combo = nullptr;
    HWND present_mode_combo = nullptr;
    HWND pacing_mode_combo = nullptr;
    HWND flow_scale_combo = nullptr;
    HWND performance_mode_checkbox = nullptr;
    HWND temporal_prior_checkbox = nullptr;
    HWND output_mode_combo = nullptr;
    HWND upscale_combo = nullptr;
    HWND debug_view_combo = nullptr;
    HWND profile_checkbox = nullptr;
    HWND profile_save_button = nullptr;
    HWND interpolator_combo = nullptr;
    HWND ui_mask_edit = nullptr;
    HWND ui_mask_hint = nullptr;
    HWND ui_mask_auto_checkbox = nullptr;
    HWND stats_checkbox = nullptr;
    HWND advanced_toggle = nullptr;
    HWND footer_hint = nullptr;
    HWND start_button = nullptr;
    HWND stop_button = nullptr;
    HWND status_text = nullptr;
    HFONT normal_font = nullptr;
    HFONT semibold_font = nullptr;
    HFONT small_font = nullptr;
    HFONT caption_font = nullptr;
    HFONT heading_font = nullptr;
    HANDLE child_process = nullptr;
    HANDLE stop_event = nullptr;
    HANDLE test_process = nullptr;
    DWORD test_process_id = 0;
    ULONGLONG test_launch_started = 0;
    osss::TestGraphicsApi pending_test_api = osss::TestGraphicsApi::direct3d11;
    int pending_test_fps = 0;
    bool closing = false;

    // Redesign state.
    LauncherPalette palette = osss::LauncherPaletteFor(false);
    bool dark = false;
    bool advanced_expanded = false;
    StatusLevel status_level = StatusLevel::ok;
    std::wstring status_headline;
    std::wstring status_detail;
    std::wstring status_note;
    // Host content heights in logical pixels, both produced by the one layout
    // walk in CreateLauncherControls.
    int collapsed_host_height = 0;
    int expanded_host_height = 0;
    // Current host viewport and scroll offset, in device pixels: the host only
    // scrolls when the expanded content cannot fit the work area, and it is the
    // only thing that scrolls -- heading, status panel, and footer stay pinned.
    int host_view_height = 0;
    int host_scroll = 0;
    RECT mask_field_rect{};
    std::vector<HWND> advanced_controls;
    std::vector<PlacedControl> placed;
    std::vector<TooltipEntry> tooltips;
    std::vector<std::unique_ptr<ControlSkin>> skins;
};

int ScaleForWindow(const HWND window, const int value) {
    return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96);
}

int UnscaleForWindow(const HWND window, const int value) {
    return MulDiv(value, 96, static_cast<int>(GetDpiForWindow(window)));
}

RECT ScaleRect(const HWND window, const RECT& rect) {
    return RECT{
        ScaleForWindow(window, rect.left),
        ScaleForWindow(window, rect.top),
        ScaleForWindow(window, rect.right),
        ScaleForWindow(window, rect.bottom),
    };
}

void SetControlFont(const HWND control, const HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void SetTextRole(const HWND control, const TextRole role) {
    SetWindowLongPtrW(control, GWLP_USERDATA, static_cast<LONG_PTR>(role));
}

TextRole TextRoleOf(const HWND control) {
    return static_cast<TextRole>(GetWindowLongPtrW(control, GWLP_USERDATA));
}

COLORREF ColorForRole(const LauncherPalette& palette, const TextRole role) {
    switch (role) {
    case TextRole::normal:
        return palette.foreground;
    case TextRole::faint:
        return palette.foreground_faint;
    case TextRole::warning:
        return palette.warning;
    case TextRole::muted:
    case TextRole::none:
    default:
        return palette.foreground_muted;
    }
}

HFONT CreateLauncherFont(const HWND window, const int height, const int weight) {
    return CreateFontW(
        -ScaleForWindow(window, height),
        0,
        0,
        0,
        weight,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
}

// Text width in logical pixels. Measured in device pixels against the font that
// will actually draw it, then converted back, because the (?) affordance sits
// at label_x + text_width + 6 and a guessed width there is how a help glyph
// ends up on top of the next column's label.
int MeasureLogical(const HWND window, const HFONT font, const std::wstring_view text) {
    const HDC dc = GetDC(window);
    if (!dc) {
        return static_cast<int>(text.size()) * 7;
    }
    const HGDIOBJ previous = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &size);
    SelectObject(dc, previous);
    ReleaseDC(window, dc);
    return UnscaleForWindow(window, size.cx) + 1;
}

void FillRounded(
    const HDC dc,
    const RECT& rect,
    const int radius,
    const COLORREF fill,
    const COLORREF border) {
    const HBRUSH brush = CreateSolidBrush(fill);
    const HPEN pen = CreatePen(PS_SOLID, 1, border);
    if (!brush || !pen) {
        if (brush) {
            DeleteObject(brush);
        }
        if (pen) {
            DeleteObject(pen);
        }
        return;
    }
    const HGDIOBJ old_brush = SelectObject(dc, brush);
    const HGDIOBJ old_pen = SelectObject(dc, pen);
    if (radius > 0) {
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    } else {
        Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    }
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void FillFlat(const HDC dc, const RECT& rect, const COLORREF color) {
    const HBRUSH brush = CreateSolidBrush(color);
    if (!brush) {
        return;
    }
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void DrawTextIn(
    const HDC dc,
    RECT rect,
    const std::wstring& text,
    const HFONT font,
    const COLORREF color,
    const UINT format) {
    const HGDIOBJ previous = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format);
    SelectObject(dc, previous);
}

int TextWidthIn(const HDC dc, const HFONT font, const std::wstring& text) {
    const HGDIOBJ previous = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, previous);
    return size.cx;
}

std::wstring WindowText(const HWND control) {
    if (!control) {
        return {};
    }
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(std::max(copied, 0)));
    return text;
}

std::wstring ComboSelectionText(const HWND combo) {
    const LRESULT selection = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR) {
        return {};
    }
    const LRESULT length = SendMessageW(combo, CB_GETLBTEXTLEN, selection, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    SendMessageW(combo, CB_GETLBTEXT, selection, reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<std::size_t>(length));
    return text;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

COLORREF Backdrop(const LauncherState& state, const ControlSkin& skin) {
    return skin.on_band ? state.palette.band : state.palette.background;
}

void PaintChevron(
    const HDC dc,
    const int centre_x,
    const int centre_y,
    const int half,
    const int thickness,
    const COLORREF color) {
    const HPEN pen = CreatePen(PS_SOLID, thickness, color);
    if (!pen) {
        return;
    }
    const HGDIOBJ previous = SelectObject(dc, pen);
    const POINT points[]{
        {centre_x - half, centre_y - half / 2},
        {centre_x, centre_y + half / 2},
        {centre_x + half, centre_y - half / 2},
    };
    Polyline(dc, points, 3);
    SelectObject(dc, previous);
    DeleteObject(pen);
}

void PaintCheckGlyph(const HDC dc, const RECT& box, const COLORREF color) {
    const int thickness = std::max(1, RectWidth(box) / 8);
    const HPEN pen = CreatePen(PS_SOLID, thickness, color);
    if (!pen) {
        return;
    }
    const HGDIOBJ previous = SelectObject(dc, pen);
    const int width = RectWidth(box);
    const POINT points[]{
        {box.left + width / 4, box.top + width / 2},
        {box.left + width * 7 / 16, box.top + width * 11 / 16},
        {box.left + width * 3 / 4, box.top + width * 5 / 16},
    };
    Polyline(dc, points, 3);
    SelectObject(dc, previous);
    DeleteObject(pen);
}

void PaintFocusRing(const HDC dc, const RECT& rect, const int radius, const COLORREF color) {
    const HPEN pen = CreatePen(PS_SOLID, 1, color);
    const HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const HGDIOBJ old_pen = SelectObject(dc, pen);
    if (radius > 0) {
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    } else {
        Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    }
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
}

COLORREF HoverFill(const LauncherState& state, const COLORREF base, const bool hot) {
    if (!hot) {
        return base;
    }
    return ShadeColor(base, state.dark ? 10 : -5);
}

void PaintComboSkin(const HWND control, const ControlSkin& skin) {
    LauncherState& state = *skin.state;
    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(control, &paint);
    if (!dc) {
        return;
    }
    RECT client{};
    GetClientRect(control, &client);
    const bool enabled = IsWindowEnabled(control) != FALSE;
    const bool focused = GetFocus() == control;
    const int radius = ScaleForWindow(control, 4);
    const COLORREF fill = enabled
        ? HoverFill(state, state.palette.field, skin.hot)
        : ShadeColor(state.palette.field, state.dark ? -25 : -4);
    const COLORREF border = enabled && focused ? state.palette.accent : state.palette.field_line;

    // The rounded corners let the parent through, so the corner pixels have to
    // be painted before the field is.
    FillFlat(dc, client, Backdrop(state, skin));
    FillRounded(dc, client, radius, fill, border);

    RECT text_rect = client;
    text_rect.left += ScaleForWindow(control, 10);
    text_rect.right -= ScaleForWindow(control, 28);
    DrawTextIn(
        dc,
        text_rect,
        ComboSelectionText(control),
        state.normal_font,
        enabled ? state.palette.foreground : state.palette.foreground_faint,
        DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    PaintChevron(
        dc,
        client.right - ScaleForWindow(control, 15),
        (client.top + client.bottom) / 2,
        ScaleForWindow(control, 4),
        std::max(1, ScaleForWindow(control, 1)),
        enabled ? state.palette.foreground_muted : state.palette.foreground_faint);
    if (enabled && focused) {
        RECT ring = client;
        InflateRect(&ring, -1, -1);
        PaintFocusRing(dc, ring, radius, state.palette.accent);
    }
    EndPaint(control, &paint);
}

void PaintButtonSkin(const HWND control, const ControlSkin& skin) {
    LauncherState& state = *skin.state;
    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(control, &paint);
    if (!dc) {
        return;
    }
    RECT client{};
    GetClientRect(control, &client);
    const bool enabled = IsWindowEnabled(control) != FALSE;
    const LRESULT button_state = SendMessageW(control, BM_GETSTATE, 0, 0);
    const bool pressed = (button_state & BST_PUSHED) != 0;
    const bool focused = (button_state & BST_FOCUS) != 0;
    const bool accent = skin.kind == SkinKind::accent_button;
    const int radius = ScaleForWindow(control, 4);

    COLORREF fill = accent ? state.palette.accent : state.palette.surface;
    COLORREF border = accent ? state.palette.accent : state.palette.field_line;
    COLORREF text = accent ? state.palette.accent_foreground : state.palette.foreground;
    if (!enabled) {
        fill = accent ? ShadeColor(state.palette.accent, state.dark ? -45 : 45)
                      : ShadeColor(state.palette.surface, state.dark ? -12 : -4);
        border = state.palette.field_line;
        text = state.palette.foreground_faint;
    } else if (pressed) {
        fill = ShadeColor(fill, state.dark ? -12 : -10);
        border = accent ? fill : border;
    } else if (skin.hot) {
        fill = ShadeColor(fill, accent ? (state.dark ? -8 : 12) : (state.dark ? 10 : -5));
        border = accent ? fill : state.palette.accent;
    }

    FillFlat(dc, client, Backdrop(state, skin));
    FillRounded(dc, client, radius, fill, border);
    DrawTextIn(
        dc,
        client,
        WindowText(control),
        state.normal_font,
        text,
        DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (enabled && focused) {
        RECT ring = client;
        InflateRect(&ring, -2, -2);
        PaintFocusRing(dc, ring, radius, accent ? state.palette.accent_foreground : state.palette.accent);
    }
    EndPaint(control, &paint);
}

void PaintCheckboxSkin(const HWND control, const ControlSkin& skin) {
    LauncherState& state = *skin.state;
    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(control, &paint);
    if (!dc) {
        return;
    }
    RECT client{};
    GetClientRect(control, &client);
    const bool enabled = IsWindowEnabled(control) != FALSE;
    const LRESULT button_state = SendMessageW(control, BM_GETSTATE, 0, 0);
    const bool checked = SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool focused = (button_state & BST_FOCUS) != 0;
    const int side = ScaleForWindow(control, 16);
    const int radius = ScaleForWindow(control, 3);

    FillFlat(dc, client, Backdrop(state, skin));

    RECT box{
        client.left,
        (client.top + client.bottom - side) / 2,
        client.left + side,
        (client.top + client.bottom + side) / 2,
    };
    COLORREF box_fill = checked ? state.palette.accent : state.palette.field;
    COLORREF box_border = checked ? state.palette.accent : state.palette.field_line;
    if (!enabled) {
        box_fill = checked ? ShadeColor(state.palette.accent, state.dark ? -45 : 45)
                           : ShadeColor(state.palette.field, state.dark ? -25 : -4);
        box_border = state.palette.field_line;
    } else if (skin.hot) {
        box_fill = HoverFill(state, box_fill, true);
        box_border = state.palette.accent;
    }
    FillRounded(dc, box, radius, box_fill, box_border);
    if (checked) {
        PaintCheckGlyph(
            dc,
            box,
            enabled ? state.palette.accent_foreground
                    : ShadeColor(state.palette.accent_foreground, state.dark ? 20 : -20));
    }

    RECT text_rect = client;
    text_rect.left = box.right + ScaleForWindow(control, 8);
    DrawTextIn(
        dc,
        text_rect,
        WindowText(control),
        state.normal_font,
        enabled ? state.palette.foreground : state.palette.foreground_faint,
        DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (enabled && focused) {
        RECT ring = text_rect;
        ring.left -= ScaleForWindow(control, 3);
        ring.right = std::min(
            ring.right,
            ring.left + TextWidthIn(dc, state.normal_font, WindowText(control)) +
                ScaleForWindow(control, 6));
        PaintFocusRing(dc, ring, 0, state.palette.accent);
    }
    EndPaint(control, &paint);
}

void PaintDisclosureSkin(const HWND control, const ControlSkin& skin) {
    LauncherState& state = *skin.state;
    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(control, &paint);
    if (!dc) {
        return;
    }
    RECT client{};
    GetClientRect(control, &client);
    const LRESULT button_state = SendMessageW(control, BM_GETSTATE, 0, 0);
    const bool focused = (button_state & BST_FOCUS) != 0;
    const int radius = ScaleForWindow(control, 4);
    const COLORREF fill = HoverFill(state, state.palette.band, skin.hot || (button_state & BST_PUSHED) != 0);

    FillFlat(dc, client, Backdrop(state, skin));
    FillRounded(dc, client, radius, fill, state.palette.line);

    // The caret is drawn rather than typed: ▶ and ▼ are not in every Segoe UI
    // fallback chain, and a missing-glyph box in the one control that explains
    // where the other twelve settings went is not a good trade.
    const int caret_x = client.left + ScaleForWindow(control, 18);
    const int caret_y = (client.top + client.bottom) / 2;
    const int caret = ScaleForWindow(control, 4);
    const HBRUSH caret_brush = CreateSolidBrush(state.palette.foreground_muted);
    if (caret_brush) {
        const HGDIOBJ old_brush = SelectObject(dc, caret_brush);
        const HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
        const POINT down[]{
            {caret_x - caret, caret_y - caret / 2},
            {caret_x + caret + 1, caret_y - caret / 2},
            {caret_x, caret_y + caret + 1},
        };
        const POINT right[]{
            {caret_x - caret / 2, caret_y - caret},
            {caret_x - caret / 2, caret_y + caret + 1},
            {caret_x + caret + 1, caret_y},
        };
        Polygon(dc, state.advanced_expanded ? down : right, 3);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
        DeleteObject(caret_brush);
    }

    const std::wstring title = L"Advanced settings";
    const std::wstring hint = state.advanced_expanded
        ? L"pacing, quality, HUD masks, diagnostics"
        : L"14 more: pacing, quality, HUD masks, diagnostics";
    RECT title_rect = client;
    title_rect.left += ScaleForWindow(control, 30);
    DrawTextIn(
        dc,
        title_rect,
        title,
        state.semibold_font,
        state.palette.foreground,
        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    RECT hint_rect = title_rect;
    hint_rect.left += TextWidthIn(dc, state.semibold_font, title) + ScaleForWindow(control, 12);
    hint_rect.right -= ScaleForWindow(control, 12);
    if (hint_rect.left < hint_rect.right) {
        DrawTextIn(
            dc,
            hint_rect,
            hint,
            state.small_font,
            state.palette.foreground_muted,
            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (focused) {
        RECT ring = client;
        InflateRect(&ring, -2, -2);
        PaintFocusRing(dc, ring, radius, state.palette.accent);
    }
    EndPaint(control, &paint);
}

LRESULT CALLBACK SkinnedControlProcedure(
    const HWND control,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam,
    const UINT_PTR identifier,
    const DWORD_PTR reference) {
    auto* const skin = reinterpret_cast<ControlSkin*>(reference);
    if (!skin || !skin->state) {
        return DefSubclassProc(control, message, wparam, lparam);
    }

    switch (message) {
    case WM_NCDESTROY:
        RemoveWindowSubclass(control, SkinnedControlProcedure, identifier);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        switch (skin->kind) {
        case SkinKind::combo:
            PaintComboSkin(control, *skin);
            return 0;
        case SkinKind::checkbox:
            PaintCheckboxSkin(control, *skin);
            return 0;
        case SkinKind::disclosure:
            PaintDisclosureSkin(control, *skin);
            return 0;
        case SkinKind::push_button:
        case SkinKind::accent_button:
        default:
            PaintButtonSkin(control, *skin);
            return 0;
        }
    case WM_MOUSEMOVE:
        if (!skin->hot) {
            skin->hot = true;
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, control, 0};
            TrackMouseEvent(&tracking);
            InvalidateRect(control, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        skin->hot = false;
        InvalidateRect(control, nullptr, FALSE);
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case BM_SETCHECK:
    case CB_SETCURSEL:
        // Let the control update itself first, then repaint from the new state.
        {
            const LRESULT result = DefSubclassProc(control, message, wparam, lparam);
            InvalidateRect(control, nullptr, FALSE);
            return result;
        }
    default:
        break;
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

// The drop-down list is its own window, so its background is not reached by the
// combo's paint. Owner-drawn items cover the rows; this covers everything below
// the last one.
LRESULT CALLBACK ComboListProcedure(
    const HWND list,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam,
    const UINT_PTR identifier,
    const DWORD_PTR reference) {
    auto* const state = reinterpret_cast<LauncherState*>(reference);
    switch (message) {
    case WM_NCDESTROY:
        RemoveWindowSubclass(list, ComboListProcedure, identifier);
        break;
    case WM_ERASEBKGND:
        if (state) {
            RECT client{};
            GetClientRect(list, &client);
            FillFlat(reinterpret_cast<HDC>(wparam), client, state->palette.field);
            return 1;
        }
        break;
    default:
        break;
    }
    return DefSubclassProc(list, message, wparam, lparam);
}

void DrawComboItem(const LauncherState& state, const DRAWITEMSTRUCT& item) {
    if (item.itemID == static_cast<UINT>(-1)) {
        return;
    }
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    FillFlat(
        item.hDC,
        item.rcItem,
        selected ? state.palette.accent : state.palette.field);
    const LRESULT length = SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, item.itemID, 0);
    std::wstring text;
    if (length > 0) {
        text.assign(static_cast<std::size_t>(length) + 1, L'\0');
        SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(text.data()));
        text.resize(static_cast<std::size_t>(length));
    }
    RECT text_rect = item.rcItem;
    text_rect.left += ScaleForWindow(item.hwndItem, 10);
    text_rect.right -= ScaleForWindow(item.hwndItem, 6);
    DrawTextIn(
        item.hDC,
        text_rect,
        text,
        state.normal_font,
        selected ? state.palette.accent_foreground : state.palette.foreground,
        DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

void DrawStatusPanel(const LauncherState& state, const DRAWITEMSTRUCT& item) {
    const RECT panel = item.rcItem;
    const HWND control = item.hwndItem;
    FillFlat(item.hDC, panel, state.palette.background);
    FillRounded(
        item.hDC,
        panel,
        ScaleForWindow(control, 6),
        state.palette.surface,
        state.palette.line);

    const COLORREF dot = state.status_level == StatusLevel::failure ? state.palette.failure
        : state.status_level == StatusLevel::warning                ? state.palette.warning
                                                                    : state.palette.ok;
    const int dot_size = ScaleForWindow(control, 8);
    const int dot_left = panel.left + ScaleForWindow(control, 14);
    const int dot_top = panel.top + ScaleForWindow(control, 18);
    const HBRUSH dot_brush = CreateSolidBrush(dot);
    if (dot_brush) {
        const HGDIOBJ old_brush = SelectObject(item.hDC, dot_brush);
        const HGDIOBJ old_pen = SelectObject(item.hDC, GetStockObject(NULL_PEN));
        Ellipse(item.hDC, dot_left, dot_top, dot_left + dot_size + 1, dot_top + dot_size + 1);
        SelectObject(item.hDC, old_pen);
        SelectObject(item.hDC, old_brush);
        DeleteObject(dot_brush);
    }

    const int text_left = panel.left + ScaleForWindow(control, 30);
    RECT headline{
        text_left,
        panel.top + ScaleForWindow(control, 12),
        panel.right - ScaleForWindow(control, 12),
        panel.top + ScaleForWindow(control, 34),
    };
    DrawTextIn(
        item.hDC,
        headline,
        state.status_headline,
        state.semibold_font,
        state.palette.foreground,
        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    RECT detail = headline;
    detail.left += TextWidthIn(item.hDC, state.semibold_font, state.status_headline) +
        ScaleForWindow(control, 10);
    if (detail.left < detail.right && !state.status_detail.empty()) {
        DrawTextIn(
            item.hDC,
            detail,
            state.status_detail,
            state.normal_font,
            state.palette.foreground_muted,
            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    if (!state.status_note.empty()) {
        RECT note{
            text_left,
            panel.top + ScaleForWindow(control, 36),
            panel.right - ScaleForWindow(control, 12),
            panel.bottom - ScaleForWindow(control, 6),
        };
        DrawTextIn(
            item.hDC,
            note,
            state.status_note,
            state.small_font,
            state.palette.foreground_faint,
            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}

// ---------------------------------------------------------------------------
// Theme, control creation, tooltips
// ---------------------------------------------------------------------------

void ApplyTooltipTheme(const LauncherState& state) {
    if (!state.tooltip) {
        return;
    }
    // TTM_SETTIPBKCOLOR is ignored while the tooltip is themed, so a dark tip
    // has to opt out of visual styles first. The light theme keeps them,
    // because the themed tooltip is already the design's --tipbg.
    SetWindowTheme(state.tooltip, state.dark ? L"" : nullptr, nullptr);
    if (state.dark) {
        SendMessageW(
            state.tooltip,
            TTM_SETTIPBKCOLOR,
            static_cast<WPARAM>(state.palette.tooltip_background),
            0);
        SendMessageW(
            state.tooltip,
            TTM_SETTIPTEXTCOLOR,
            static_cast<WPARAM>(state.palette.tooltip_foreground),
            0);
    }
}

void ApplyTheme(LauncherState& state, const bool dark) {
    state.dark = dark;
    state.palette = osss::LauncherPaletteFor(dark);
    const BOOL immersive = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(state.window, DWMWA_USE_IMMERSIVE_DARK_MODE, &immersive, sizeof(immersive));
    if (state.ui_mask_edit) {
        SetWindowTheme(state.ui_mask_edit, dark ? L"DarkMode_CFD" : nullptr, nullptr);
    }
    ApplyTooltipTheme(state);
    RedrawWindow(
        state.window,
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

ControlSkin* AttachSkin(
    LauncherState& state,
    const HWND control,
    const SkinKind kind,
    const bool on_band = false) {
    if (!control) {
        return nullptr;
    }
    auto skin = std::make_unique<ControlSkin>();
    skin->kind = kind;
    skin->state = &state;
    skin->on_band = on_band;
    ControlSkin* const raw = skin.get();
    state.skins.push_back(std::move(skin));
    SetWindowSubclass(
        control,
        SkinnedControlProcedure,
        reinterpret_cast<UINT_PTR>(raw),
        reinterpret_cast<DWORD_PTR>(raw));
    return raw;
}

void RecordPlacement(
    LauncherState& state,
    const HWND parent,
    const HWND control,
    const RECT& rect,
    const bool advanced) {
    if (!control) {
        return;
    }
    state.placed.push_back(PlacedControl{parent, control, rect, advanced});
    if (advanced) {
        state.advanced_controls.push_back(control);
    }
}

HWND CreateControlIn(
    LauncherState& state,
    const HWND parent,
    const DWORD extended_style,
    const wchar_t* const class_name,
    const wchar_t* const text,
    const DWORD style,
    const RECT& rect,
    const HFONT font,
    const int identifier = 0,
    const int extra_height = 0) {
    const RECT scaled = ScaleRect(parent, rect);
    const HWND control = CreateWindowExW(
        extended_style,
        class_name,
        text,
        WS_CHILD | WS_VISIBLE | style,
        scaled.left,
        scaled.top,
        scaled.right - scaled.left,
        scaled.bottom - scaled.top + ScaleForWindow(parent, extra_height),
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
        GetModuleHandleW(nullptr),
        nullptr);
    if (control) {
        SetControlFont(control, font ? font : state.normal_font);
    }
    return control;
}

// A drop-down list combo. The extra height is the dropped list: a combo box
// takes the height of the whole dropped control at creation and sizes its
// closed self from the item height, so the rect recorded for the overlap check
// is the closed rect, not what was passed to CreateWindowEx.
HWND CreateComboIn(
    LauncherState& state,
    const HWND parent,
    const RECT& rect,
    const int dropdown_height,
    const int identifier,
    const bool advanced) {
    const HWND combo = CreateControlIn(
        state,
        parent,
        0,
        L"COMBOBOX",
        L"",
        CBS_DROPDOWNLIST | CBS_HASSTRINGS | CBS_OWNERDRAWFIXED | WS_VSCROLL | WS_TABSTOP,
        rect,
        state.normal_font,
        identifier,
        dropdown_height);
    if (!combo) {
        return nullptr;
    }
    SendMessageW(
        combo,
        CB_SETITEMHEIGHT,
        static_cast<WPARAM>(-1),
        ScaleForWindow(parent, RectHeight(rect) - 6));
    SendMessageW(combo, CB_SETITEMHEIGHT, 0, ScaleForWindow(parent, 24));
    AttachSkin(state, combo, SkinKind::combo);
    COMBOBOXINFO info{sizeof(info)};
    if (GetComboBoxInfo(combo, &info) && info.hwndList) {
        SetWindowSubclass(
            info.hwndList,
            ComboListProcedure,
            reinterpret_cast<UINT_PTR>(&state),
            reinterpret_cast<DWORD_PTR>(&state));
    }
    RecordPlacement(state, parent, combo, rect, advanced);
    return combo;
}

HWND CreateCheckboxIn(
    LauncherState& state,
    const HWND parent,
    const RECT& row,
    const wchar_t* const text,
    const int identifier,
    const bool advanced) {
    // Sized to its own text: a checkbox that claims the whole row would sit on
    // top of the hint static placed after it, which is exactly the overlap the
    // self-test now refuses.
    RECT rect = row;
    rect.right = std::min(
        row.right,
        row.left + 24 + MeasureLogical(parent, state.normal_font, text) + 8);
    const HWND control = CreateControlIn(
        state,
        parent,
        0,
        L"BUTTON",
        text,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        rect,
        state.normal_font,
        identifier);
    AttachSkin(state, control, SkinKind::checkbox);
    RecordPlacement(state, parent, control, rect, advanced);
    return control;
}

// A checkbox's qualifier, in the small font, after its label. The design splits
// "Cheaper motion search" from "measured: little faster, much softer" for one
// reason: as a single line of checkbox text it was 420 px of string in a 252 px
// control, clipped mid-word.
HWND CreateTrailingHint(
    LauncherState& state,
    const HWND parent,
    const HWND after,
    const RECT& row,
    const wchar_t* const text,
    const bool advanced) {
    RECT anchor{};
    for (const auto& placed : state.placed) {
        if (placed.control == after) {
            anchor = placed.rect;
        }
    }
    RECT rect = row;
    rect.left = anchor.right + 4;
    rect.right = std::min(row.right, rect.left + MeasureLogical(parent, state.small_font, text) + 4);
    if (rect.left >= rect.right) {
        return nullptr;
    }
    const HWND hint = CreateControlIn(
        state,
        parent,
        0,
        L"STATIC",
        text,
        SS_LEFT | SS_CENTERIMAGE,
        rect,
        state.small_font);
    SetTextRole(hint, TextRole::faint);
    RecordPlacement(state, parent, hint, rect, advanced);
    return hint;
}

// A section caption plus the rule that runs from it to the content edge.
void CreateSectionCaption(
    LauncherState& state,
    const HWND parent,
    LauncherLayout& layout,
    const wchar_t* const text,
    const bool advanced) {
    const RECT row = layout.Row(LauncherLayout::kCaptionHeight);
    RECT caption = row;
    caption.right = std::min(row.right, row.left + MeasureLogical(parent, state.caption_font, text) + 4);
    const HWND label = CreateControlIn(
        state, parent, 0, L"STATIC", text, SS_LEFT, caption, state.caption_font);
    SetTextRole(label, TextRole::faint);
    RecordPlacement(state, parent, label, caption, advanced);

    RECT rule{caption.right + 8, row.top + 8, row.right, row.top + 9};
    if (rule.left < rule.right) {
        const HWND line = CreateControlIn(
            state, parent, 0, L"STATIC", L"", SS_ETCHEDHORZ, rule, state.small_font);
        RecordPlacement(state, parent, line, rule, advanced);
    }
    layout.Gap(LauncherLayout::kCaptionGap);
}

struct FieldLabel {
    HWND label = nullptr;
    HWND help = nullptr;
};

FieldLabel CreateFieldLabel(
    LauncherState& state,
    const HWND parent,
    const RECT& column,
    const wchar_t* const text,
    const bool advanced) {
    FieldLabel result;
    RECT label_rect = column;
    label_rect.right = std::min(
        column.right, column.left + MeasureLogical(parent, state.small_font, text) + 2);
    result.label = CreateControlIn(
        state, parent, 0, L"STATIC", text, SS_LEFT | SS_NOTIFY, label_rect, state.small_font);
    SetTextRole(result.label, TextRole::muted);
    RecordPlacement(state, parent, result.label, label_rect, advanced);

    RECT help_rect = column;
    help_rect.left = label_rect.right + 6;
    help_rect.right = help_rect.left + MeasureLogical(parent, state.small_font, L"(?)") + 2;
    if (help_rect.right <= column.right) {
        result.help = CreateControlIn(
            state, parent, 0, L"STATIC", L"(?)", SS_LEFT | SS_NOTIFY, help_rect, state.small_font);
        SetTextRole(result.help, TextRole::faint);
        RecordPlacement(state, parent, result.help, help_rect, advanced);
    }
    return result;
}

void AddTip(LauncherState& state, const HWND control, const Tip tip) {
    if (!control || !state.tooltip) {
        return;
    }
    TTTOOLINFOW info{};
    info.cbSize = sizeof(info);
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = GetParent(control);
    info.uId = reinterpret_cast<UINT_PTR>(control);
    info.lpszText = LPSTR_TEXTCALLBACKW;
    if (SendMessageW(state.tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info))) {
        state.tooltips.push_back(TooltipEntry{control, tip});
    }
}

// Registers the tip on the control, its label, and its (?) glyph, so hovering
// any of the three shows it.
void AddFieldTip(
    LauncherState& state,
    const FieldLabel& label,
    const HWND control,
    const Tip tip) {
    AddTip(state, control, tip);
    AddTip(state, label.label, tip);
    AddTip(state, label.help, tip);
}

bool HandleTooltipNotify(const LauncherState& state, const LPARAM lparam) {
    const auto* const header = reinterpret_cast<NMHDR*>(lparam);
    if (!header || header->code != TTN_GETDISPINFOW) {
        return false;
    }
    auto* const info = reinterpret_cast<NMTTDISPINFOW*>(lparam);
    const auto control = reinterpret_cast<HWND>(header->idFrom);
    for (const TooltipEntry& entry : state.tooltips) {
        if (entry.control != control) {
            continue;
        }
        const TipText& text = kTips[static_cast<std::size_t>(entry.tip)];
        // Re-sent per display: TTM_SETTITLE belongs to the tooltip window, not
        // to a tool, so the last tool to show would otherwise own the title.
        SendMessageW(
            state.tooltip,
            TTM_SETTITLE,
            TTI_NONE,
            reinterpret_cast<LPARAM>(text.title));
        info->lpszText = const_cast<wchar_t*>(text.body);
        info->hinst = nullptr;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Status panel
// ---------------------------------------------------------------------------

void SetStatus(
    LauncherState& state,
    const StatusLevel level,
    std::wstring headline,
    std::wstring detail,
    std::wstring note = {}) {
    state.status_level = level;
    state.status_headline = std::move(headline);
    state.status_detail = std::move(detail);
    state.status_note = std::move(note);
    if (!state.status_text) {
        return;
    }
    // The window text is still the whole sentence: the self-test and anything
    // else reading this control keep working, and the panel draws from the
    // three parts.
    std::wstring combined = state.status_headline;
    if (!state.status_detail.empty()) {
        combined += L": " + state.status_detail;
    }
    SetWindowTextW(state.status_text, combined.c_str());
    InvalidateRect(state.status_text, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

int WorkAreaHeightLogical(const HWND window) {
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) {
        return 1080;
    }
    return UnscaleForWindow(window, info.rcWork.bottom - info.rcWork.top);
}

// The client height the launcher wants, and the height it may actually have.
// Both are derived from the layout walk rather than written down, so adding a
// row cannot leave a stale window height behind it.
int WantedClientHeight(const LauncherState& state, const bool expanded) {
    const int content = expanded ? state.expanded_host_height : state.collapsed_host_height;
    return kHostTop + content + kBelowHost;
}

int ClampClientHeight(const int wanted, const int work_area_height) {
    return std::min(wanted, std::max(work_area_height - kWorkAreaReserve, 360));
}

void UpdateHostScrollInfo(LauncherState& state) {
    const int content = ScaleForWindow(
        state.window,
        state.advanced_expanded ? state.expanded_host_height : state.collapsed_host_height);
    const bool scrolls = content > state.host_view_height;
    const LONG_PTR style = GetWindowLongPtrW(state.settings_host, GWL_STYLE);
    const LONG_PTR wanted = scrolls ? (style | WS_VSCROLL) : (style & ~static_cast<LONG_PTR>(WS_VSCROLL));
    if (style != wanted) {
        SetWindowLongPtrW(state.settings_host, GWL_STYLE, wanted);
        SetWindowPos(
            state.settings_host,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    SCROLLINFO scroll{sizeof(scroll)};
    scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    scroll.nMin = 0;
    scroll.nMax = std::max(content - 1, 0);
    scroll.nPage = static_cast<UINT>(std::max(state.host_view_height, 1));
    const int maximum_scroll = std::max(content - state.host_view_height, 0);
    state.host_scroll = std::clamp(state.host_scroll, 0, maximum_scroll);
    scroll.nPos = state.host_scroll;
    SetScrollInfo(state.settings_host, SB_VERT, &scroll, TRUE);
}

void ScrollHostTo(LauncherState& state, const int position) {
    const int content = ScaleForWindow(
        state.window,
        state.advanced_expanded ? state.expanded_host_height : state.collapsed_host_height);
    const int maximum_scroll = std::max(content - state.host_view_height, 0);
    const int clamped = std::clamp(position, 0, maximum_scroll);
    const int delta = state.host_scroll - clamped;
    if (delta == 0) {
        return;
    }
    state.host_scroll = clamped;
    ScrollWindowEx(
        state.settings_host,
        0,
        delta,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    SCROLLINFO scroll{sizeof(scroll)};
    scroll.fMask = SIF_POS;
    scroll.nPos = clamped;
    SetScrollInfo(state.settings_host, SB_VERT, &scroll, TRUE);
    UpdateWindow(state.settings_host);
}

void ApplyGeometry(LauncherState& state) {
    if (!state.window || !state.settings_host) {
        return;
    }
    const int wanted = WantedClientHeight(state, state.advanced_expanded);
    const int client_height = ClampClientHeight(wanted, WorkAreaHeightLogical(state.window));
    const int host_height = std::max(client_height - kHostTop - kBelowHost, 40);

    RECT bounds{0, 0, ScaleForWindow(state.window, kWindowWidth), ScaleForWindow(state.window, client_height)};
    AdjustWindowRectExForDpi(
        &bounds,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        TRUE,
        0,
        GetDpiForWindow(state.window));
    SetWindowPos(
        state.window,
        nullptr,
        0,
        0,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    state.host_view_height = ScaleForWindow(state.window, host_height);
    SetWindowPos(
        state.settings_host,
        nullptr,
        0,
        ScaleForWindow(state.window, kHostTop),
        ScaleForWindow(state.window, kWindowWidth),
        state.host_view_height,
        SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateHostScrollInfo(state);
    ScrollHostTo(state, state.host_scroll);

    const int status_top = kHostTop + host_height + kStatusGap;
    const RECT status{
        LauncherLayout::kContentLeft,
        status_top,
        LauncherLayout::kContentRight,
        status_top + kStatusHeight,
    };
    const RECT scaled_status = ScaleRect(state.window, status);
    SetWindowPos(
        state.status_text,
        nullptr,
        scaled_status.left,
        scaled_status.top,
        RectWidth(scaled_status),
        RectHeight(scaled_status),
        SWP_NOZORDER | SWP_NOACTIVATE);

    const int band_top = status_top + kStatusHeight + kFooterGap;
    const auto place = [&state](const HWND control, const RECT& rect) {
        if (!control) {
            return;
        }
        const RECT scaled = ScaleRect(state.window, rect);
        SetWindowPos(
            control,
            nullptr,
            scaled.left,
            scaled.top,
            RectWidth(scaled),
            RectHeight(scaled),
            SWP_NOZORDER | SWP_NOACTIVATE);
    };
    place(state.footer_hint, RECT{24, band_top + 17, 424, band_top + 47});
    place(state.stop_button, RECT{494, band_top + 15, 584, band_top + 49});
    place(state.start_button, RECT{596, band_top + 15, 696, band_top + 49});
    InvalidateRect(state.window, nullptr, TRUE);
}


std::wstring ExecutablePath(const wchar_t* const file_name) {
    std::array<wchar_t, 32768> module_path{};
    const DWORD length = GetModuleFileNameW(
        nullptr,
        module_path.data(),
        static_cast<DWORD>(module_path.size()));
    if (length == 0 || length >= module_path.size()) {
        return {};
    }

    std::wstring path(module_path.data(), length);
    const std::size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return file_name;
    }
    path.resize(separator + 1);
    path.append(file_name);
    return path;
}

std::wstring ExecutableDirectory() {
    const std::wstring executable = ExecutablePath(L"");
    if (executable.empty()) {
        return {};
    }
    return executable;
}

std::wstring LastWindowsError(const DWORD code) {
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        reinterpret_cast<wchar_t*>(&message),
        0,
        nullptr);
    std::wstring result = length > 0 && message ? std::wstring(message, length) : L"Unknown Windows error";
    if (message) {
        LocalFree(message);
    }
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
        result.pop_back();
    }
    return result;
}

HWND SelectedTarget(const LauncherState& state) {
    const LRESULT selection = SendMessageW(state.target_combo, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR) {
        return nullptr;
    }
    const LRESULT item_data = SendMessageW(state.target_combo, CB_GETITEMDATA, selection, 0);
    if (item_data == CB_ERR) {
        return nullptr;
    }
    return reinterpret_cast<HWND>(item_data);
}

std::optional<double> SelectedTargetFps(const LauncherState& state, const HWND target) {
    const LRESULT selection = SendMessageW(state.target_fps_combo, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR) {
        return std::nullopt;
    }
    const LRESULT item_data = SendMessageW(state.target_fps_combo, CB_GETITEMDATA, selection, 0);
    if (item_data == CB_ERR) {
        return std::nullopt;
    }
    if (item_data > 0) {
        return static_cast<double>(item_data);
    }
    return osss::WindowDisplayRefreshRate(target);
}

int SelectedMaxMultiplier(const LauncherState& state) {
    const LRESULT selection = SendMessageW(
        state.max_multiplier_combo,
        CB_GETCURSEL,
        0,
        0);
    return selection == CB_ERR
        ? osss::kMaximumMultiplier
        : static_cast<int>(selection) + osss::kMinimumMultiplier;
}

int SelectedBufferMilliseconds(const LauncherState& state) {
    const LRESULT selection = SendMessageW(state.buffer_combo, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR) {
        return osss::FrameSelector::kDefaultBufferMilliseconds;
    }
    const LRESULT item_data = SendMessageW(state.buffer_combo, CB_GETITEMDATA, selection, 0);
    return item_data == CB_ERR
        ? osss::FrameSelector::kDefaultBufferMilliseconds
        : static_cast<int>(item_data);
}

osss::FrameSelector::CeilingPacing SelectedCeilingPacing(const LauncherState& state) {
    const LRESULT selection = SendMessageW(state.ceiling_pacing_combo, CB_GETCURSEL, 0, 0);
    return selection == 1
        ? osss::FrameSelector::CeilingPacing::spread
        : osss::FrameSelector::CeilingPacing::even;
}


osss::DebugView SelectedDebugView(const LauncherState& state) {
    const LRESULT selection = SendMessageW(state.debug_view_combo, CB_GETCURSEL, 0, 0);
    switch (selection) {
    case 1:
        return osss::DebugView::flow;
    case 2:
        return osss::DebugView::confidence;
    case 3:
        return osss::DebugView::fallback;
    default:
        return osss::DebugView::off;
    }
}

osss::UpscaleMode SelectedUpscaleMode(const LauncherState& state) {
    const LRESULT selection = SendMessageW(state.upscale_combo, CB_GETCURSEL, 0, 0);
    switch (selection) {
    case 1:
        return osss::UpscaleMode::off;
    case 2:
        return osss::UpscaleMode::always;
    default:
        return osss::UpscaleMode::automatic;
    }
}

osss::OutputMode SelectedOutputMode(const LauncherState& state) {
    const LRESULT selection = SendMessageW(state.output_mode_combo, CB_GETCURSEL, 0, 0);
    return selection == 1 ? osss::OutputMode::fullscreen : osss::OutputMode::overlay;
}

osss::FlowScale SelectedFlowScale(const LauncherState& state) {
    const LRESULT selection = SendMessageW(state.flow_scale_combo, CB_GETCURSEL, 0, 0);
    switch (selection) {
    case 1:
        return osss::FlowScale::quality;
    case 2:
        return osss::FlowScale::performance;
    case 3:
        return osss::FlowScale::ultra_performance;
    default:
        return osss::FlowScale::automatic;
    }
}

osss::PresentMode SelectedPresentMode(const LauncherState& state) {
    const LRESULT selection = SendMessageW(state.present_mode_combo, CB_GETCURSEL, 0, 0);
    switch (selection) {
    case 1:
        return osss::PresentMode::tearing;
    case 2:
        return osss::PresentMode::vsync;
    default:
        return osss::PresentMode::automatic;
    }
}

osss::PacingMode SelectedPacingMode(const LauncherState& state) {
    const LRESULT selection = SendMessageW(state.pacing_mode_combo, CB_GETCURSEL, 0, 0);
    switch (selection) {
    case 1:
        return osss::PacingMode::queued;
    case 2:
        return osss::PacingMode::unpaced;
    default:
        return osss::PacingMode::paced;
    }
}

std::wstring UiMaskText(const LauncherState& state) {
    if (!state.ui_mask_edit) {
        return {};
    }
    const int length = GetWindowTextLengthW(state.ui_mask_edit);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(state.ui_mask_edit, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(std::max(copied, 0)));
    return text;
}

// The tunables, as an argument list, with no --title/--hwnd/--stop-event: those
// name a session, not a setting, and a profile that pinned them would apply the
// wrong target and a dead event handle on every later run.
//
// Deliberately does not include --profile itself. A profile that re-applied a
// profile is either a no-op or a loop, and neither is worth supporting.
//
// This list has to stay in step with the launch command line built in
// StartCapture. RunLauncherSelfTest asserts every flag emitted here also
// appears there, which is the guard against the two drifting.
std::vector<std::wstring> ProfileArgumentsFor(const LauncherState& state) {
    std::vector<std::wstring> arguments;
    const auto push = [&arguments](std::wstring flag, std::wstring value) {
        arguments.push_back(std::move(flag));
        arguments.push_back(std::move(value));
    };
    if (const auto fps = SelectedTargetFps(state, SelectedTarget(state))) {
        push(L"--target-fps", std::to_wstring(static_cast<int>(*fps)));
    }
    push(L"--max-multiplier", std::to_wstring(SelectedMaxMultiplier(state)));
    push(L"--buffer", std::to_wstring(SelectedBufferMilliseconds(state)));
    push(
        L"--ceiling-pacing",
        SelectedCeilingPacing(state) == osss::FrameSelector::CeilingPacing::even ? L"even"
                                                                                : L"spread");
    push(L"--present-mode", osss::PresentModeArgument(SelectedPresentMode(state)));
    push(L"--pacing", osss::PacingModeArgument(SelectedPacingMode(state)));
    push(L"--flow-scale", osss::FlowScaleArgument(SelectedFlowScale(state)));
    push(
        L"--performance-mode",
        SendMessageW(state.performance_mode_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED
            ? L"on"
            : L"off");
    push(
        L"--temporal-prior",
        SendMessageW(state.temporal_prior_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED
            ? L"on"
            : L"off");
    push(
        L"--interpolator",
        SendMessageW(state.interpolator_combo, CB_GETCURSEL, 0, 0) == 1 ? L"blend" : L"motion");
    push(L"--output-mode", osss::OutputModeArgument(SelectedOutputMode(state)));
    push(L"--upscale", osss::UpscaleModeArgument(SelectedUpscaleMode(state)));
    push(L"--debug-view", osss::DebugViewArgument(SelectedDebugView(state)));
    push(
        L"--stats-overlay",
        SendMessageW(state.stats_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"on" : L"off");
    const std::wstring mask = UiMaskText(state);
    if (!mask.empty()) {
        push(L"--ui-mask", mask);
    }
    push(
        L"--ui-mask-auto",
        SendMessageW(state.ui_mask_auto_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"on"
                                                                                   : L"off");
    return arguments;
}

// Executable file name for a window, via the catalog that already collects it.
std::wstring ExecutableNameFor(const HWND target) {
    for (const osss::WindowEntry& entry : osss::ListCapturableWindows()) {
        if (entry.handle == target) {
            return entry.process_name;
        }
    }
    return {};
}

void UpdateReadyStatus(LauncherState& state) {
    if (state.child_process) {
        return;
    }
    const HWND target = SelectedTarget(state);
    if (!target) {
        SetStatus(
            state,
            StatusLevel::warning,
            L"No target",
            L"No visible, restored windows found. Open one and select Refresh.");
        return;
    }
    const auto target_fps = SelectedTargetFps(state, target);
    if (!target_fps) {
        SetStatus(
            state,
            StatusLevel::warning,
            L"No display rate",
            L"Choose a manual output target.");
        return;
    }
    // Data, not prose: one line of resolved values, and the rule that relates
    // them on a second, dimmer one. The sentence this replaced was ~1000 px of
    // text in a 552 px field.
    const std::wstring separator = L"  ·  ";
    SetStatus(
        state,
        StatusLevel::ok,
        L"Ready",
        std::to_wstring(static_cast<int>(*target_fps)) + L" FPS target" + separator +
            L"ceiling " + std::to_wstring(SelectedMaxMultiplier(state)) + L"x" + separator +
            std::to_wstring(SelectedBufferMilliseconds(state)) + L" ms buffer floor" +
            separator +
            (SelectedCeilingPacing(state) == osss::FrameSelector::CeilingPacing::even
                ? L"even cadence when bound"
                : L"distributed cadence"),
        L"Resulting output is min(target, source × ceiling).");
}

void UpdateControlAvailability(const LauncherState& state) {
    const bool running = state.child_process != nullptr;
    const bool launching_test = state.test_process != nullptr;
    EnableWindow(state.target_combo, !running);
    EnableWindow(state.refresh_button, !running);
    EnableWindow(state.target_fps_combo, !running);
    EnableWindow(state.max_multiplier_combo, !running);
    EnableWindow(state.buffer_combo, !running);
    EnableWindow(state.ceiling_pacing_combo, !running);
    EnableWindow(state.present_mode_combo, !running);
    EnableWindow(state.pacing_mode_combo, !running);
    EnableWindow(state.flow_scale_combo, !running);
    EnableWindow(state.performance_mode_checkbox, !running);
    EnableWindow(state.temporal_prior_checkbox, !running);
    EnableWindow(state.output_mode_combo, !running);
    EnableWindow(state.upscale_combo, !running);
    EnableWindow(state.debug_view_combo, !running);
    EnableWindow(state.profile_checkbox, !running);
    EnableWindow(state.profile_save_button, !running);
    EnableWindow(state.interpolator_combo, !running);
    EnableWindow(state.ui_mask_edit, !running);
    EnableWindow(state.ui_mask_auto_checkbox, !running);
    EnableWindow(state.stats_checkbox, !running);
    EnableWindow(
        state.start_button,
        !running && !launching_test && SelectedTarget(state) != nullptr);
    EnableWindow(state.stop_button, running);
}

// Start compiling the motion shaders before the user has finished choosing a
// target.
//
// The shaders are inline HLSL compiled at runtime, and the first compile costs
// several seconds -- almost all of it in a single entry point. osss.exe caches
// the resulting bytecode per user, so only the first run after an update pays,
// but that first run used to be a session the user was waiting on. Picking a
// window and setting options takes longer than the compile does, so doing it
// here means the wait usually never happens at all.
//
// Fire and forget: the handle is closed immediately. A failure is not worth
// reporting, because the only consequence is that the next start compiles the
// shaders itself, exactly as it did before.
void WarmShaderCache() {
    const std::wstring cli_path = ExecutablePath(L"osss.exe");
    if (cli_path.empty() || GetFileAttributesW(cli_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    std::wstring command = L'"' + cli_path + L"\" --warm-shader-cache";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring working_directory = ExecutableDirectory();
    if (!CreateProcessW(
            cli_path.c_str(),
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
            nullptr,
            working_directory.empty() ? nullptr : working_directory.c_str(),
            &startup,
            &process)) {
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

void PopulateTargets(LauncherState& state) {
    const HWND prior_selection = SelectedTarget(state);
    const HWND foreground = GetForegroundWindow();
    const DWORD own_process = GetCurrentProcessId();
    SendMessageW(state.target_combo, CB_RESETCONTENT, 0, 0);

    int prior_index = -1;
    int foreground_index = -1;
    int first_index = -1;
    for (const auto& entry : osss::ListCapturableWindows()) {
        if (entry.process_id == own_process || entry.handle == state.window) {
            continue;
        }

        // Executable first: the combo is far narrower than a typical title, so whatever
        // leads has to be the part that identifies the app.
        const std::wstring display =
            (entry.process_name.empty() ? L"(unknown)" : entry.process_name) +
            L"  \u2014  " + entry.title +
            L"  (PID " + std::to_wstring(entry.process_id) + L")";
        const LRESULT index = SendMessageW(
            state.target_combo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(display.c_str()));
        if (index == CB_ERR || index == CB_ERRSPACE) {
            continue;
        }
        SendMessageW(
            state.target_combo,
            CB_SETITEMDATA,
            index,
            reinterpret_cast<LPARAM>(entry.handle));
        if (first_index < 0) {
            first_index = static_cast<int>(index);
        }
        if (entry.handle == prior_selection) {
            prior_index = static_cast<int>(index);
        }
        if (entry.handle == foreground) {
            foreground_index = static_cast<int>(index);
        }
    }

    const int selection = prior_index >= 0
        ? prior_index
        : (foreground_index >= 0 ? foreground_index : first_index);
    if (selection >= 0) {
        SendMessageW(state.target_combo, CB_SETCURSEL, selection, 0);
    }
    UpdateReadyStatus(state);
    UpdateControlAvailability(state);
}

bool SelectTargetInCombo(LauncherState& state, const HWND target) {
    const LRESULT count = SendMessageW(state.target_combo, CB_GETCOUNT, 0, 0);
    for (LRESULT index = 0; index < count; ++index) {
        if (reinterpret_cast<HWND>(
                SendMessageW(state.target_combo, CB_GETITEMDATA, index, 0)) == target) {
            SendMessageW(state.target_combo, CB_SETCURSEL, index, 0);
            UpdateReadyStatus(state);
            UpdateControlAvailability(state);
            return true;
        }
    }
    return false;
}

HWND FindWindowForProcess(const DWORD process_id) {
    for (const auto& entry : osss::ListCapturableWindows()) {
        if (entry.process_id == process_id) {
            return entry.handle;
        }
    }
    return nullptr;
}

bool DecodeTestAnimationCommand(
    const int command,
    osss::TestGraphicsApi& api,
    int& base_fps) {
    if (command < kTestAnimationMenuFirst || command > kTestAnimationMenuLast) {
        return false;
    }
    const int offset = command - kTestAnimationMenuFirst;
    const auto rate_count = static_cast<int>(osss::kTestAnimationBaseRates.size());
    api = osss::kTestGraphicsApis[static_cast<std::size_t>(offset / rate_count)];
    base_fps = osss::kTestAnimationBaseRates[static_cast<std::size_t>(offset % rate_count)];
    return true;
}

void LaunchTestAnimation(
    LauncherState& state,
    const osss::TestGraphicsApi api,
    const int base_fps) {
    if (state.child_process) {
        SetStatus(
            state,
            StatusLevel::warning,
            L"Busy",
            L"Stop frame generation before changing to another test source.");
        return;
    }
    if (state.test_process) {
        SetStatus(
            state,
            StatusLevel::warning,
            L"Busy",
            L"The previous test animation is still starting. Please wait.");
        return;
    }

    const std::wstring executable = ExecutablePath(L"osss_test_animation.exe");
    if (executable.empty() || GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) {
        SetStatus(
            state,
            StatusLevel::failure,
            L"Missing",
            L"osss_test_animation.exe was not found beside this launcher. Rebuild the project.");
        return;
    }

    std::wostringstream command;
    command
        << L'"' << executable << L"\" --api " << osss::TestGraphicsApiArgument(api)
        << L" --fps " << base_fps;
    std::wstring mutable_command = command.str();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring working_directory = ExecutableDirectory();
    const BOOL created = CreateProcessW(
        executable.c_str(),
        mutable_command.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        working_directory.empty() ? nullptr : working_directory.c_str(),
        &startup,
        &process);
    if (!created) {
        SetStatus(
            state,
            StatusLevel::failure,
            L"Test failed",
            LastWindowsError(GetLastError()));
        return;
    }

    CloseHandle(process.hThread);
    state.test_process = process.hProcess;
    state.test_process_id = process.dwProcessId;
    state.test_launch_started = GetTickCount64();
    state.pending_test_api = api;
    state.pending_test_fps = base_fps;
    SetStatus(
        state,
        StatusLevel::ok,
        L"Launching",
        std::wstring(osss::TestGraphicsApiDisplayName(api)) + L" test at " +
            std::to_wstring(base_fps) + L" FPS...");
    UpdateControlAvailability(state);
}

void FinishPendingTestLaunch(LauncherState& state) {
    if (state.test_process) {
        CloseHandle(state.test_process);
        state.test_process = nullptr;
    }
    state.test_process_id = 0;
    state.test_launch_started = 0;
    state.pending_test_fps = 0;
    UpdateControlAvailability(state);
}

void PollTestAnimation(LauncherState& state) {
    if (!state.test_process) {
        return;
    }

    const HWND animation_window = FindWindowForProcess(state.test_process_id);
    if (animation_window) {
        const osss::TestGraphicsApi api = state.pending_test_api;
        const int base_fps = state.pending_test_fps;
        PopulateTargets(state);
        FinishPendingTestLaunch(state);
        if (SelectTargetInCombo(state, animation_window)) {
            SetStatus(
                state,
                StatusLevel::ok,
                L"Test ready",
                std::wstring(osss::TestGraphicsApiDisplayName(api)) + L" at " +
                    std::to_wstring(base_fps) + L" FPS is selected. Choose the output target, then Start.",
                L"Press S in the test window to score the visible output.");
            SetForegroundWindow(animation_window);
            return;
        }
        SetStatus(
            state,
            StatusLevel::warning,
            L"Test launched",
            std::wstring(osss::TestGraphicsApiDisplayName(api)) + L" at " +
                std::to_wstring(base_fps) + L" FPS, but selection failed. Select Refresh.");
        return;
    }

    if (WaitForSingleObject(state.test_process, 0) == WAIT_OBJECT_0) {
        DWORD exit_code = 1;
        GetExitCodeProcess(state.test_process, &exit_code);
        FinishPendingTestLaunch(state);
        SetStatus(
            state,
            StatusLevel::failure,
            L"Test failed",
            L"It exited before its window was ready (error " + std::to_wstring(exit_code) +
                L"). Run it in a terminal for details.");
        return;
    }

    if (GetTickCount64() - state.test_launch_started >= 5000) {
        const auto api = state.pending_test_api;
        const int base_fps = state.pending_test_fps;
        FinishPendingTestLaunch(state);
        PopulateTargets(state);
        SetStatus(
            state,
            StatusLevel::warning,
            L"Test launched",
            std::wstring(osss::TestGraphicsApiDisplayName(api)) + L" at " +
                std::to_wstring(base_fps) +
                L" FPS, but Windows did not expose it in time. Select Refresh.");
    }
}

void CloseChildHandles(LauncherState& state) {
    if (state.child_process) {
        CloseHandle(state.child_process);
        state.child_process = nullptr;
    }
    if (state.stop_event) {
        CloseHandle(state.stop_event);
        state.stop_event = nullptr;
    }
}

void RequestStop(LauncherState& state) {
    if (!state.stop_event) {
        return;
    }
    if (!SetEvent(state.stop_event)) {
        SetStatus(
            state,
            StatusLevel::failure,
            L"Stop failed",
            LastWindowsError(GetLastError()));
        return;
    }
    SetStatus(state, StatusLevel::ok, L"Stopping", L"Ending the session.");
    EnableWindow(state.stop_button, FALSE);
}

void PollChild(LauncherState& state) {
    if (!state.child_process) {
        return;
    }

    const DWORD process_state = WaitForSingleObject(state.child_process, 0);
    if (process_state == WAIT_TIMEOUT) {
        // The generated-frame surface is also topmost and is created after the
        // launcher. Reassert the launcher within the topmost band so Stop stays
        // visible and reachable while generation is active.
        SetWindowPos(
            state.window,
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return;
    }
    if (process_state != WAIT_OBJECT_0) {
        return;
    }

    DWORD exit_code = 1;
    GetExitCodeProcess(state.child_process, &exit_code);
    CloseChildHandles(state);
    SetWindowPos(
        state.window,
        HWND_NOTOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (state.closing) {
        DestroyWindow(state.window);
        return;
    }

    PopulateTargets(state);
    if (exit_code == 0) {
        SetStatus(state, StatusLevel::ok, L"Stopped", L"Ready to start again.");
    } else {
        SetStatus(
            state,
            StatusLevel::failure,
            L"Exited",
            L"Frame generation exited with error code " + std::to_wstring(exit_code) +
                L". Run osss.exe in a terminal for details.");
    }
}

// Writes the current settings as the profile for the selected target, keyed by
// its executable. Deliberately stores what ProfileArgumentsFor emits rather
// than the launch command line: --title, --hwnd, and --stop-event name a
// session, not a setting.
void SaveTargetProfile(LauncherState& state) {
    const HWND target = SelectedTarget(state);
    if (!target || !IsWindow(target)) {
        SetStatus(
            state,
            StatusLevel::warning,
            L"Profile",
            L"Select a target window before saving a profile.");
        return;
    }
    const std::wstring executable = ExecutableNameFor(target);
    if (executable.empty()) {
        SetStatus(
            state,
            StatusLevel::failure,
            L"Profile",
            L"That target's executable name could not be read.");
        return;
    }
    auto profiles = osss::LoadProfiles();
    if (!profiles.Ok()) {
        SetStatus(
            state,
            StatusLevel::failure,
            L"Profile",
            L"The file could not be read (line " + std::to_wstring(profiles.error_line) +
                L"): " + profiles.error);
        return;
    }
    osss::SetProfileArguments(profiles.entries, executable, ProfileArgumentsFor(state));
    std::wstring error;
    if (!osss::SaveProfiles(profiles.entries, error)) {
        SetStatus(state, StatusLevel::failure, L"Profile", L"Could not be saved: " + error);
        return;
    }
    SetStatus(state, StatusLevel::ok, L"Saved", L"Profile written for " + executable + L".");
}

void BeginGeneration(LauncherState& state) {
    if (state.child_process) {
        return;
    }

    const HWND target = SelectedTarget(state);
    if (!target || !IsWindow(target) || IsIconic(target)) {
        SetStatus(
            state,
            StatusLevel::warning,
            L"No target",
            L"That target is no longer available. Select Refresh.");
        PopulateTargets(state);
        return;
    }

    const auto target_fps = SelectedTargetFps(state, target);
    if (!target_fps) {
        SetStatus(
            state,
            StatusLevel::warning,
            L"No display rate",
            L"Could not read that display's refresh rate. Choose a manual output target.");
        return;
    }

    const int max_multiplier = SelectedMaxMultiplier(state);
    const int buffer_milliseconds = SelectedBufferMilliseconds(state);
    const auto ceiling_pacing = SelectedCeilingPacing(state);
    const auto present_mode = SelectedPresentMode(state);
    const auto pacing_mode = SelectedPacingMode(state);
    const auto flow_scale = SelectedFlowScale(state);
    const bool performance_mode =
        SendMessageW(state.performance_mode_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool temporal_prior =
        SendMessageW(state.temporal_prior_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const auto output_mode = SelectedOutputMode(state);
    const auto upscale_mode = SelectedUpscaleMode(state);
    const auto debug_view = SelectedDebugView(state);
    const bool apply_profile =
        SendMessageW(state.profile_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const LRESULT interpolator_selection = SendMessageW(
        state.interpolator_combo,
        CB_GETCURSEL,
        0,
        0);
    const wchar_t* const interpolator = interpolator_selection == 1 ? L"blend" : L"motion";
    const bool show_stats =
        SendMessageW(state.stats_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;

    // Validate the HUD mask here so a typo surfaces in the launcher instead of
    // as a silent child-process exit code.
    const std::wstring ui_mask_text = UiMaskText(state);
    const auto ui_mask = osss::ParseUiMaskRects(ui_mask_text);
    if (!ui_mask.Ok()) {
        SetStatus(
            state,
            StatusLevel::warning,
            L"HUD mask",
            std::wstring(ui_mask.error.begin(), ui_mask.error.end()));
        SetFocus(state.ui_mask_edit);
        return;
    }
    const std::wstring ui_mask_argument = osss::FormatUiMaskRects(ui_mask.rects);
    const bool ui_mask_auto =
        SendMessageW(state.ui_mask_auto_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;

    const std::wstring cli_path = ExecutablePath(L"osss.exe");
    if (cli_path.empty() || GetFileAttributesW(cli_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        SetStatus(
            state,
            StatusLevel::failure,
            L"Missing",
            L"osss.exe was not found beside this launcher. Rebuild both targets.");
        return;
    }

    const std::wstring stop_event_name =
        L"Local\\OSSS.Stop." + std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64());
    state.stop_event = CreateEventW(nullptr, TRUE, FALSE, stop_event_name.c_str());
    if (!state.stop_event) {
        SetStatus(
            state,
            StatusLevel::failure,
            L"Start failed",
            L"Could not create the local Stop signal: " + LastWindowsError(GetLastError()));
        return;
    }

    std::wostringstream command;
    const LRESULT target_selection = SendMessageW(
        state.target_fps_combo,
        CB_GETCURSEL,
        0,
        0);
    command
        << L'"' << cli_path << L"\" --hwnd 0x"
        << std::hex << reinterpret_cast<std::uintptr_t>(target)
        << std::dec << L" --target-fps ";
    if (target_selection == 0) {
        command << L"auto";
    } else {
        command << static_cast<int>(*target_fps);
    }
    command
        << L" --max-multiplier " << max_multiplier
        << L" --buffer " << buffer_milliseconds
        << L" --ceiling-pacing "
        << (ceiling_pacing == osss::FrameSelector::CeilingPacing::even ? L"even" : L"spread")
        << L" --present-mode " << osss::PresentModeArgument(present_mode)
        << L" --pacing " << osss::PacingModeArgument(pacing_mode)
        << L" --flow-scale " << osss::FlowScaleArgument(flow_scale)
        << L" --performance-mode " << (performance_mode ? L"on" : L"off")
        << L" --temporal-prior " << (temporal_prior ? L"on" : L"off")
        << L" --output-mode " << osss::OutputModeArgument(output_mode)
        << L" --upscale " << osss::UpscaleModeArgument(upscale_mode)
        << L" --debug-view " << osss::DebugViewArgument(debug_view)
        << (apply_profile ? L" --profile auto" : L"")
        << L" --interpolator " << interpolator
        << L" --stats-overlay " << (show_stats ? L"on" : L"off");
    if (!ui_mask_argument.empty()) {
        // FormatUiMaskRects emits only digits, '.', ',', ';', spaces, and "px",
        // so the quoted argument cannot break CreateProcess parsing.
        command << L" --ui-mask \"" << ui_mask_argument << L'"';
    }
    if (ui_mask_auto) {
        command << L" --ui-mask-auto on";
    }
    command << L" --stop-event \"" << stop_event_name << L'"';
    std::wstring mutable_command = command.str();

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring working_directory = ExecutableDirectory();
    const BOOL created = CreateProcessW(
        cli_path.c_str(),
        mutable_command.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        working_directory.empty() ? nullptr : working_directory.c_str(),
        &startup,
        &process);
    if (!created) {
        const DWORD error = GetLastError();
        CloseChildHandles(state);
        SetStatus(
            state,
            StatusLevel::failure,
            L"Start failed",
            LastWindowsError(error));
        return;
    }

    CloseHandle(process.hThread);
    state.child_process = process.hProcess;

    // Hand the foreground back to the target, and get out of the way.
    //
    // This used to pin the launcher topmost and re-show it. The effect was that
    // the thing the user had just asked to accelerate was left in the
    // background, underneath the launcher -- and osss.exe deliberately keeps its
    // overlay hidden while the target does not own the foreground, so nothing
    // appeared to happen at all until the user manually minimized the launcher
    // and clicked the game. That read as "OSSS takes forever to hook".
    //
    // SetForegroundWindow is allowed here because the launcher *is* the
    // foreground process at this instant -- the user just clicked Start -- which
    // is exactly the condition Windows grants the call on.
    SetWindowPos(
        state.window,
        HWND_NOTOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    // Let the child raise the target too: it inherits this permission only if it
    // is granted before it needs it, and a game that re-creates its window on
    // resume would otherwise be stuck behind whatever has the foreground.
    AllowSetForegroundWindow(process.dwProcessId);
    ShowWindow(state.window, SW_MINIMIZE);
    SetForegroundWindow(target);
    BringWindowToTop(target);
    const std::wstring target_title = osss::WindowTitle(target);
    SetStatus(
        state,
        StatusLevel::ok,
        L"Running",
        std::to_wstring(static_cast<int>(*target_fps)) + L" FPS, up to " +
            std::to_wstring(max_multiplier) + L"x " +
            (interpolator_selection == 1 ? L"Blend" : L"Motion") +
            (ui_mask.rects.empty()
                ? (ui_mask_auto ? L", auto HUD mask" : L"")
                : L", " + std::to_wstring(ui_mask.rects.size()) + L" HUD mask region" +
                    (ui_mask.rects.size() == 1 ? L"" : L"s") +
                    (ui_mask_auto ? L" + auto" : L"")) +
            L" on " + target_title,
        L"The stop and generation-toggle hotkeys are printed in the OSSS startup "
        L"banner; this Stop button always works.");
    UpdateControlAvailability(state);
    SetForegroundWindow(target);
}

// ---------------------------------------------------------------------------
// Persisted launcher preferences
// ---------------------------------------------------------------------------

// Beside profiles.txt, and deliberately not inside it: profiles are the command
// line you would have typed for one program, and whether a disclosure is open
// is neither a flag nor per-program.
std::filesystem::path LauncherPreferencesPath() {
    const auto profiles = osss::ProfilePath();
    if (profiles.empty()) {
        return {};
    }
    return profiles.parent_path() / L"launcher.txt";
}

bool LoadAdvancedExpanded() {
    const auto path = LauncherPreferencesPath();
    if (path.empty()) {
        return false;
    }
    std::wifstream file(path);
    if (!file) {
        return false;
    }
    std::wstring line;
    while (std::getline(file, line)) {
        if (line.rfind(L"advanced=", 0) == 0) {
            return line.substr(9) == L"1";
        }
    }
    return false;
}

void SaveAdvancedExpanded(const bool expanded) {
    const auto path = LauncherPreferencesPath();
    if (path.empty()) {
        return;
    }
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);
    std::wofstream file(path, std::ios::trunc);
    if (!file) {
        return;
    }
    file << L"# OSSS launcher window state. Settings live in profiles.txt.\n"
         << L"advanced=" << (expanded ? L'1' : L'0') << L'\n';
}

// ---------------------------------------------------------------------------
// HUD mask parse feedback
// ---------------------------------------------------------------------------

constexpr wchar_t kMaskHintDefault[] =
    L"left,top,right,bottom per region; 0-1 fractions or px; separate with ;";

void UpdateMaskHint(LauncherState& state) {
    if (!state.ui_mask_hint) {
        return;
    }
    const std::wstring text = UiMaskText(state);
    if (text.empty()) {
        SetTextRole(state.ui_mask_hint, TextRole::faint);
        SetWindowTextW(state.ui_mask_hint, kMaskHintDefault);
        InvalidateRect(state.ui_mask_hint, nullptr, TRUE);
        UpdateReadyStatus(state);
        return;
    }
    const auto parsed = osss::ParseUiMaskRects(text);
    if (!parsed.Ok()) {
        const std::wstring error(parsed.error.begin(), parsed.error.end());
        SetTextRole(state.ui_mask_hint, TextRole::warning);
        SetWindowTextW(state.ui_mask_hint, error.c_str());
        InvalidateRect(state.ui_mask_hint, nullptr, TRUE);
        SetStatus(state, StatusLevel::warning, L"HUD mask", error);
        return;
    }
    std::wstring summary = std::to_wstring(parsed.rects.size()) + L" region" +
        (parsed.rects.size() == 1 ? L"" : L"s") + L" parsed.";
    SetTextRole(state.ui_mask_hint, TextRole::faint);
    SetWindowTextW(state.ui_mask_hint, summary.c_str());
    InvalidateRect(state.ui_mask_hint, nullptr, TRUE);
    UpdateReadyStatus(state);
}

// ---------------------------------------------------------------------------
// The layout walk
// ---------------------------------------------------------------------------

void AddComboItems(
    const HWND combo,
    const std::initializer_list<const wchar_t*> items,
    const int selection) {
    for (const wchar_t* const item : items) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }
    SendMessageW(combo, CB_SETCURSEL, selection, 0);
}

void SetAdvancedExpanded(LauncherState& state, const bool expanded, const bool persist) {
    state.advanced_expanded = expanded;
    for (const HWND control : state.advanced_controls) {
        ShowWindow(control, expanded ? SW_SHOW : SW_HIDE);
    }
    if (!expanded) {
        state.host_scroll = 0;
    }
    if (state.advanced_toggle) {
        InvalidateRect(state.advanced_toggle, nullptr, TRUE);
    }
    ApplyGeometry(state);
    if (persist) {
        SaveAdvancedExpanded(expanded);
    }
}

bool CreateLauncherControls(LauncherState& state) {
    const HWND window = state.window;
    state.normal_font = CreateLauncherFont(window, 14, FW_NORMAL);
    state.semibold_font = CreateLauncherFont(window, 14, FW_SEMIBOLD);
    state.small_font = CreateLauncherFont(window, 13, FW_NORMAL);
    state.caption_font = CreateLauncherFont(window, 12, FW_SEMIBOLD);
    state.heading_font = CreateLauncherFont(window, 22, FW_SEMIBOLD);
    if (!state.normal_font || !state.semibold_font || !state.small_font ||
        !state.caption_font || !state.heading_font) {
        return false;
    }

    state.dark = osss::SystemPrefersDarkApps();
    state.palette = osss::LauncherPaletteFor(state.dark);
    state.advanced_expanded = LoadAdvancedExpanded();

    const HWND heading = CreateControlIn(
        state, window, 0, L"STATIC", L"Frame Generation", SS_LEFT,
        RECT{24, 20, 424, 50}, state.heading_font);
    SetTextRole(heading, TextRole::normal);
    RecordPlacement(state, window, heading, RECT{24, 20, 424, 50}, false);
    const HWND subtitle = CreateControlIn(
        state, window, 0, L"STATIC",
        L"Choose a window, output target, and interpolation limit.", SS_LEFT,
        RECT{24, 52, 524, 72}, state.small_font);
    SetTextRole(subtitle, TextRole::muted);
    RecordPlacement(state, window, subtitle, RECT{24, 52, 524, 72}, false);

    // Only the settings scroll. The heading, the status panel, and the footer
    // band stay pinned, because Start has to be reachable at every window
    // height the work-area clamp can produce.
    state.settings_host = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kSettingsHostClassName,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0,
        ScaleForWindow(window, kHostTop),
        ScaleForWindow(window, kWindowWidth),
        ScaleForWindow(window, 320),
        window,
        nullptr,
        GetModuleHandleW(nullptr),
        &state);
    if (!state.settings_host) {
        return false;
    }
    const HWND host = state.settings_host;

    state.tooltip = CreateWindowExW(
        WS_EX_TOPMOST,
        TOOLTIPS_CLASSW,
        nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        window,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    if (state.tooltip) {
        SetControlFont(state.tooltip, state.small_font);
        // Without a maximum tip width every tip is one very long line; this is
        // what turns them into the wrapped paragraphs the copy was written as.
        SendMessageW(state.tooltip, TTM_SETMAXTIPWIDTH, 0, ScaleForWindow(window, 330));
        SendMessageW(state.tooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 420);
        SendMessageW(state.tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
    }

    LauncherLayout layout(8);

    // --- Target ------------------------------------------------------------
    CreateSectionCaption(state, host, layout, L"TARGET", false);
    const RECT target_label_row = layout.Column(0, 1, LauncherLayout::kLabelHeight);
    const FieldLabel target_label =
        CreateFieldLabel(state, host, target_label_row, L"Target window", false);
    layout.Gap(LauncherLayout::kLabelHeight + LauncherLayout::kLabelGap);
    RECT target_rect = layout.Column(0, 1, LauncherLayout::kControlHeight);
    target_rect.right = 580;
    state.target_combo = CreateComboIn(state, host, target_rect, 250, kTargetCombo, false);
    SendMessageW(state.target_combo, CB_SETDROPPEDWIDTH, ScaleForWindow(window, 900), 0);
    const RECT refresh_rect{592, target_rect.top, 696, target_rect.bottom};
    state.refresh_button = CreateControlIn(
        state, host, 0, L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP,
        refresh_rect, state.normal_font, kRefreshButton);
    AttachSkin(state, state.refresh_button, SkinKind::push_button);
    RecordPlacement(state, host, state.refresh_button, refresh_rect, false);
    layout.Gap(LauncherLayout::kControlHeight + LauncherLayout::kSectionGap);

    // --- Output ------------------------------------------------------------
    CreateSectionCaption(state, host, layout, L"OUTPUT", false);
    const FieldLabel fps_label = CreateFieldLabel(
        state, host, layout.Column(0, 2, LauncherLayout::kLabelHeight), L"Output target", false);
    const FieldLabel multiplier_label = CreateFieldLabel(
        state, host, layout.Column(1, 2, LauncherLayout::kLabelHeight),
        L"Maximum interpolation", false);
    layout.Gap(LauncherLayout::kLabelHeight + LauncherLayout::kLabelGap);
    state.target_fps_combo = CreateComboIn(
        state, host, layout.Column(0, 2, LauncherLayout::kControlHeight), 260, kTargetFpsCombo, false);
    state.max_multiplier_combo = CreateComboIn(
        state, host, layout.Column(1, 2, LauncherLayout::kControlHeight), 220, kMultiplierCombo, false);
    layout.Gap(LauncherLayout::kControlHeight + LauncherLayout::kRowGap);

    const LRESULT automatic_target = SendMessageW(
        state.target_fps_combo, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(L"Match display (recommended)"));
    if (automatic_target != CB_ERR && automatic_target != CB_ERRSPACE) {
        SendMessageW(state.target_fps_combo, CB_SETITEMDATA, automatic_target, 0);
    }
    for (const int target_fps : kTargetFpsOptions) {
        const std::wstring label = std::to_wstring(target_fps) + L" FPS";
        const LRESULT index = SendMessageW(
            state.target_fps_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (index != CB_ERR && index != CB_ERRSPACE) {
            SendMessageW(state.target_fps_combo, CB_SETITEMDATA, index, target_fps);
        }
    }
    SendMessageW(state.target_fps_combo, CB_SETCURSEL, 0, 0);
    for (int multiplier = osss::kMinimumMultiplier; multiplier <= osss::kMaximumMultiplier;
         ++multiplier) {
        const std::wstring label = L"Up to " + std::to_wstring(multiplier) + L"x";
        SendMessageW(
            state.max_multiplier_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    SendMessageW(state.max_multiplier_combo, CB_SETCURSEL, 4, 0);

    const FieldLabel interpolator_label = CreateFieldLabel(
        state, host, layout.Column(0, 2, LauncherLayout::kLabelHeight), L"Interpolator", false);
    layout.Gap(LauncherLayout::kLabelHeight + LauncherLayout::kLabelGap);
    state.interpolator_combo = CreateComboIn(
        state, host, layout.Column(0, 2, LauncherLayout::kControlHeight), 140, kInterpolatorCombo, false);
    AddComboItems(
        state.interpolator_combo,
        {L"Motion aware (recommended)", L"Temporal blend (comparison)"},
        0);
    {
        // Vertically centred on the 32 px control band beside it.
        RECT stats_row = layout.Column(1, 2, LauncherLayout::kCheckboxHeight);
        stats_row.top += 4;
        stats_row.bottom += 4;
        state.stats_checkbox = CreateCheckboxIn(
            state, host, stats_row, L"Show source/output FPS overlay", kStatsCheckbox, false);
        SendMessageW(state.stats_checkbox, BM_SETCHECK, BST_CHECKED, 0);
    }
    layout.Gap(LauncherLayout::kControlHeight + kDisclosureGap);

    // --- Advanced disclosure ------------------------------------------------
    const RECT disclosure = layout.Row(kDisclosureHeight);
    state.advanced_toggle = CreateControlIn(
        state, host, 0, L"BUTTON", L"Advanced settings", BS_PUSHBUTTON | WS_TABSTOP,
        disclosure, state.normal_font, kAdvancedToggle);
    AttachSkin(state, state.advanced_toggle, SkinKind::disclosure);
    RecordPlacement(state, host, state.advanced_toggle, disclosure, false);
    state.collapsed_host_height = layout.Cursor() + 12;
    layout.Gap(kAdvancedTop);

    // --- Pacing -------------------------------------------------------------
    // Four controls, not the design's three: --pacing landed after the design
    // was written, so this is a 2x2 block on the two-column grid rather than a
    // single three-column row.
    CreateSectionCaption(state, host, layout, L"PACING", true);
    const FieldLabel present_label = CreateFieldLabel(
        state, host, layout.Column(0, 2, LauncherLayout::kLabelHeight), L"Present mode", true);
    const FieldLabel pacing_label = CreateFieldLabel(
        state, host, layout.Column(1, 2, LauncherLayout::kLabelHeight), L"Pacing", true);
    layout.Gap(LauncherLayout::kLabelHeight + LauncherLayout::kLabelGap);
    state.present_mode_combo = CreateComboIn(
        state, host, layout.Column(0, 2, LauncherLayout::kControlHeight), 140, kPresentModeCombo, true);
    state.pacing_mode_combo = CreateComboIn(
        state, host, layout.Column(1, 2, LauncherLayout::kControlHeight), 140, kPacingModeCombo, true);
    layout.Gap(LauncherLayout::kControlHeight + LauncherLayout::kRowGap);
    // Order matches SelectedPresentMode: automatic, tearing, vsync.
    AddComboItems(state.present_mode_combo, {L"Auto", L"Tearing / VRR", L"VSync"}, 0);
    // Order matches SelectedPacingMode: paced, queued, unpaced.
    AddComboItems(state.pacing_mode_combo, {L"Paced", L"Queued", L"Unpaced"}, 0);

    const FieldLabel buffer_label = CreateFieldLabel(
        state, host, layout.Column(0, 2, LauncherLayout::kLabelHeight), L"Adaptive buffer floor", true);
    const FieldLabel ceiling_label = CreateFieldLabel(
        state, host, layout.Column(1, 2, LauncherLayout::kLabelHeight), L"Ceiling pacing", true);
    layout.Gap(LauncherLayout::kLabelHeight + LauncherLayout::kLabelGap);
    state.buffer_combo = CreateComboIn(
        state, host, layout.Column(0, 2, LauncherLayout::kControlHeight), 200, kBufferCombo, true);
    state.ceiling_pacing_combo = CreateComboIn(
        state, host, layout.Column(1, 2, LauncherLayout::kControlHeight), 120, kCeilingPacingCombo, true);
    layout.Gap(LauncherLayout::kControlHeight + LauncherLayout::kSectionGap);
    for (const int buffer_milliseconds : kBufferFloorOptions) {
        const std::wstring label = std::to_wstring(buffer_milliseconds) + L" ms floor";
        const LRESULT index = SendMessageW(
            state.buffer_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (index != CB_ERR && index != CB_ERRSPACE) {
            SendMessageW(state.buffer_combo, CB_SETITEMDATA, index, buffer_milliseconds);
        }
    }
    SendMessageW(state.buffer_combo, CB_SETCURSEL, 2, 0);
    AddComboItems(state.ceiling_pacing_combo, {L"Even", L"Spread"}, 0);

    // --- Quality ------------------------------------------------------------
    CreateSectionCaption(state, host, layout, L"QUALITY", true);
    const FieldLabel flow_label = CreateFieldLabel(
        state, host, layout.Column(0, 2, LauncherLayout::kLabelHeight), L"Flow scale", true);
    const FieldLabel upscale_label = CreateFieldLabel(
        state, host, layout.Column(1, 2, LauncherLayout::kLabelHeight), L"Upscale", true);
    layout.Gap(LauncherLayout::kLabelHeight + LauncherLayout::kLabelGap);
    state.flow_scale_combo = CreateComboIn(
        state, host, layout.Column(0, 2, LauncherLayout::kControlHeight), 160, kFlowScaleCombo, true);
    state.upscale_combo = CreateComboIn(
        state, host, layout.Column(1, 2, LauncherLayout::kControlHeight), 140, kUpscaleCombo, true);
    layout.Gap(LauncherLayout::kControlHeight + LauncherLayout::kRowGap);
    // Order matches SelectedFlowScale: automatic, quality, performance, ultra.
    AddComboItems(
        state.flow_scale_combo,
        {L"Auto (4 up to 1440p, 8 above)", L"Quality (4 everywhere)",
         L"Performance (8 everywhere)", L"Ultra performance (16 everywhere)"},
        0);
    // Order matches SelectedUpscaleMode: automatic, off, always.
    AddComboItems(
        state.upscale_combo,
        {L"Auto (only when output is larger)", L"Off", L"Always (even at 1:1)"},
        0);

    {
        const RECT row = layout.Row(LauncherLayout::kCheckboxHeight);
        state.performance_mode_checkbox = CreateCheckboxIn(
            state, host, row, L"Cheaper motion search", kPerformanceModeCheckbox, true);
        SendMessageW(state.performance_mode_checkbox, BM_SETCHECK, BST_UNCHECKED, 0);
        CreateTrailingHint(
            state, host, state.performance_mode_checkbox, row,
            L"measured: little faster, much softer", true);
        layout.Gap(LauncherLayout::kCheckboxGap);
    }
    {
        // On by default, like the flag: it is an extra search candidate seeded
        // from the previous pair, and off exists for A/B comparison.
        const RECT row = layout.Row(LauncherLayout::kCheckboxHeight);
        state.temporal_prior_checkbox = CreateCheckboxIn(
            state, host, row, L"Temporal prior", kTemporalPriorCheckbox, true);
        SendMessageW(state.temporal_prior_checkbox, BM_SETCHECK, BST_CHECKED, 0);
        CreateTrailingHint(
            state, host, state.temporal_prior_checkbox, row,
            L"seeds each pair's search from the last one", true);
        layout.Gap(LauncherLayout::kSectionGap);
    }

    // --- Output shape and HUD masks ----------------------------------------
    CreateSectionCaption(state, host, layout, L"OUTPUT SHAPE AND HUD MASKS", true);
    const FieldLabel output_label = CreateFieldLabel(
        state, host, layout.Column(0, 2, LauncherLayout::kLabelHeight), L"Output", true);
    layout.Gap(LauncherLayout::kLabelHeight + LauncherLayout::kLabelGap);
    state.output_mode_combo = CreateComboIn(
        state, host, layout.Column(0, 2, LauncherLayout::kControlHeight), 120, kOutputModeCombo, true);
    layout.Gap(LauncherLayout::kControlHeight + LauncherLayout::kRowGap);
    // Order matches SelectedOutputMode: overlay, fullscreen.
    AddComboItems(
        state.output_mode_combo,
        {L"Overlay (click-through, no VRR)", L"Fullscreen (G-Sync / FreeSync, no clicks)"},
        0);

    const FieldLabel mask_label = CreateFieldLabel(
        state, host, layout.Column(0, 1, LauncherLayout::kLabelHeight),
        L"HUD mask regions (optional)", true);
    layout.Gap(LauncherLayout::kLabelHeight + LauncherLayout::kLabelGap);
    {
        const RECT field = layout.Row(LauncherLayout::kControlHeight);
        state.mask_field_rect = field;
        // Borderless, inset inside the frame the host paints for it: a
        // WS_EX_CLIENTEDGE edit is drawn by the system in a colour the dark
        // theme cannot reach.
        const RECT inner{field.left + 2, field.top + 6, field.right - 2, field.bottom - 6};
        state.ui_mask_edit = CreateControlIn(
            state, host, 0, L"EDIT", L"", ES_LEFT | ES_AUTOHSCROLL | WS_TABSTOP,
            inner, state.normal_font, kUiMaskEdit);
        SendMessageW(
            state.ui_mask_edit,
            EM_SETMARGINS,
            EC_LEFTMARGIN | EC_RIGHTMARGIN,
            MAKELPARAM(ScaleForWindow(host, 8), ScaleForWindow(host, 8)));
        RecordPlacement(state, host, state.ui_mask_edit, field, true);
        layout.Gap(4);
    }
    {
        const RECT row = layout.Row(17);
        state.ui_mask_hint = CreateControlIn(
            state, host, 0, L"STATIC", kMaskHintDefault, SS_LEFT | SS_CENTERIMAGE,
            row, state.small_font);
        SetTextRole(state.ui_mask_hint, TextRole::faint);
        RecordPlacement(state, host, state.ui_mask_hint, row, true);
        layout.Gap(LauncherLayout::kCheckboxGap);
    }
    {
        const RECT row = layout.Row(LauncherLayout::kCheckboxHeight);
        state.ui_mask_auto_checkbox = CreateCheckboxIn(
            state, host, row, L"Also detect static HUD regions automatically",
            kUiMaskAutoCheckbox, true);
        SendMessageW(state.ui_mask_auto_checkbox, BM_SETCHECK, BST_UNCHECKED, 0);
        CreateTrailingHint(
            state, host, state.ui_mask_auto_checkbox, row, L"experimental", true);
        layout.Gap(LauncherLayout::kSectionGap);
    }

    // --- Diagnostics and profiles ------------------------------------------
    CreateSectionCaption(state, host, layout, L"DIAGNOSTICS AND PROFILES", true);
    const FieldLabel debug_label = CreateFieldLabel(
        state, host, layout.Column(0, 2, LauncherLayout::kLabelHeight), L"Diagnostic view", true);
    layout.Gap(LauncherLayout::kLabelHeight + LauncherLayout::kLabelGap);
    state.debug_view_combo = CreateComboIn(
        state, host, layout.Column(0, 2, LauncherLayout::kControlHeight), 160, kDebugViewCombo, true);
    // Order matches SelectedDebugView: off, flow, confidence, fallback.
    AddComboItems(
        state.debug_view_combo,
        {L"Off (normal output)", L"Flow field", L"Confidence", L"Fallback reason"},
        0);
    {
        RECT profile_row = layout.Column(1, 2, LauncherLayout::kCheckboxHeight);
        profile_row.top += 4;
        profile_row.bottom += 4;
        state.profile_checkbox = CreateCheckboxIn(
            state, host, profile_row, L"Apply saved profile", kProfileCheckbox, true);
        SendMessageW(state.profile_checkbox, BM_SETCHECK, BST_UNCHECKED, 0);
        const RECT save_rect{622, profile_row.top - 4, 696, profile_row.top + 28};
        state.profile_save_button = CreateControlIn(
            state, host, 0, L"BUTTON", L"Save", BS_PUSHBUTTON | WS_TABSTOP,
            save_rect, state.normal_font, kProfileSaveButton);
        AttachSkin(state, state.profile_save_button, SkinKind::push_button);
        RecordPlacement(state, host, state.profile_save_button, save_rect, true);
    }
    layout.Gap(LauncherLayout::kControlHeight);
    state.expanded_host_height = layout.Cursor() + 12;

    // --- Status panel and footer band ---------------------------------------
    state.status_text = CreateControlIn(
        state, window, 0, L"STATIC", L"", SS_OWNERDRAW,
        RECT{24, 412, 696, 474}, state.normal_font, kStatusText);
    RecordPlacement(state, window, state.status_text, RECT{24, 412, 696, 474}, false);
    state.footer_hint = CreateControlIn(
        state, window, 0, L"STATIC",
        L"Start hands the foreground back to the target.\nCtrl+Alt+F12 stops the session.",
        SS_LEFT, RECT{24, 511, 424, 541}, state.small_font);
    SetTextRole(state.footer_hint, TextRole::faint);
    RecordPlacement(state, window, state.footer_hint, RECT{24, 511, 424, 541}, false);
    state.stop_button = CreateControlIn(
        state, window, 0, L"BUTTON", L"Stop", BS_PUSHBUTTON | WS_TABSTOP,
        RECT{494, 509, 584, 543}, state.normal_font, kStopButton);
    AttachSkin(state, state.stop_button, SkinKind::push_button, true);
    RecordPlacement(state, window, state.stop_button, RECT{494, 509, 584, 543}, false);
    state.start_button = CreateControlIn(
        state, window, 0, L"BUTTON", L"Start", BS_DEFPUSHBUTTON | WS_TABSTOP,
        RECT{596, 509, 696, 543}, state.normal_font, kStartButton);
    AttachSkin(state, state.start_button, SkinKind::accent_button, true);
    RecordPlacement(state, window, state.start_button, RECT{596, 509, 696, 543}, false);

    // --- Tooltips -----------------------------------------------------------
    AddFieldTip(state, target_label, state.target_combo, Tip::target_window);
    AddTip(state, state.refresh_button, Tip::refresh);
    AddFieldTip(state, fps_label, state.target_fps_combo, Tip::output_target);
    AddFieldTip(state, multiplier_label, state.max_multiplier_combo, Tip::maximum_interpolation);
    AddFieldTip(state, interpolator_label, state.interpolator_combo, Tip::interpolator);
    AddTip(state, state.stats_checkbox, Tip::stats_overlay);
    AddFieldTip(state, present_label, state.present_mode_combo, Tip::present_mode);
    AddFieldTip(state, pacing_label, state.pacing_mode_combo, Tip::pacing_mode);
    AddFieldTip(state, buffer_label, state.buffer_combo, Tip::buffer_floor);
    AddFieldTip(state, ceiling_label, state.ceiling_pacing_combo, Tip::ceiling_pacing);
    AddFieldTip(state, flow_label, state.flow_scale_combo, Tip::flow_scale);
    AddFieldTip(state, upscale_label, state.upscale_combo, Tip::upscale);
    AddTip(state, state.performance_mode_checkbox, Tip::performance_mode);
    AddTip(state, state.temporal_prior_checkbox, Tip::temporal_prior);
    AddFieldTip(state, output_label, state.output_mode_combo, Tip::output_shape);
    AddFieldTip(state, mask_label, state.ui_mask_edit, Tip::ui_mask);
    AddTip(state, state.ui_mask_auto_checkbox, Tip::ui_mask_auto);
    AddFieldTip(state, debug_label, state.debug_view_combo, Tip::debug_view);
    AddTip(state, state.profile_checkbox, Tip::profile);
    AddTip(state, state.profile_save_button, Tip::profile);
    AddTip(state, state.start_button, Tip::start);

    ApplyTheme(state, state.dark);
    SetAdvancedExpanded(state, state.advanced_expanded, false);
    SetStatus(state, StatusLevel::ok, L"Loading", L"Reading visible windows...");

    return heading && subtitle && state.target_combo && state.refresh_button &&
        state.target_fps_combo && state.max_multiplier_combo && state.buffer_combo &&
        state.ceiling_pacing_combo && state.present_mode_combo && state.pacing_mode_combo &&
        state.flow_scale_combo && state.performance_mode_checkbox &&
        state.temporal_prior_checkbox && state.output_mode_combo && state.upscale_combo &&
        state.debug_view_combo && state.profile_checkbox && state.profile_save_button &&
        state.interpolator_combo && state.ui_mask_edit && state.ui_mask_hint &&
        state.ui_mask_auto_checkbox && state.stats_checkbox && state.advanced_toggle &&
        state.footer_hint && state.start_button && state.stop_button && state.status_text &&
        state.tooltip;
}

// ---------------------------------------------------------------------------
// Window procedures
// ---------------------------------------------------------------------------

// One cached brush per background colour. WM_CTLCOLOR* requires a brush that
// outlives the message, so it cannot be a local, and there are only ever two
// backgrounds in this window plus the field colour.
HBRUSH CachedBrush(const COLORREF color) {
    static COLORREF cached_colors[4]{};
    static HBRUSH cached_brushes[4]{};
    for (int index = 0; index < 4; ++index) {
        if (cached_brushes[index] && cached_colors[index] == color) {
            return cached_brushes[index];
        }
    }
    for (int index = 0; index < 4; ++index) {
        if (!cached_brushes[index]) {
            cached_colors[index] = color;
            cached_brushes[index] = CreateSolidBrush(color);
            return cached_brushes[index];
        }
    }
    // A theme switch can retire the four; recycle the first slot.
    DeleteObject(cached_brushes[0]);
    cached_colors[0] = color;
    cached_brushes[0] = CreateSolidBrush(color);
    return cached_brushes[0];
}

LRESULT HandleControlColor(
    const LauncherState& state,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
    const auto dc = reinterpret_cast<HDC>(wparam);
    const auto control = reinterpret_cast<HWND>(lparam);
    if (message == WM_CTLCOLOREDIT) {
        SetTextColor(dc, state.palette.foreground);
        SetBkColor(dc, state.palette.field);
        return reinterpret_cast<LRESULT>(CachedBrush(state.palette.field));
    }
    const COLORREF backdrop =
        control == state.footer_hint ? state.palette.band : state.palette.background;
    SetTextColor(dc, ColorForRole(state.palette, TextRoleOf(control)));
    SetBkColor(dc, backdrop);
    return reinterpret_cast<LRESULT>(CachedBrush(backdrop));
}

LRESULT CALLBACK SettingsHostProcedure(
    const HWND host,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
    auto* state = reinterpret_cast<LauncherState*>(GetWindowLongPtrW(host, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* const create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<LauncherState*>(create->lpCreateParams);
        SetWindowLongPtrW(host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) {
        return DefWindowProcW(host, message, wparam, lparam);
    }

    switch (message) {
    case WM_ERASEBKGND: {
        RECT client{};
        GetClientRect(host, &client);
        FillFlat(reinterpret_cast<HDC>(wparam), client, state->palette.background);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(host, &paint);
        if (dc && state->ui_mask_edit) {
            // The edit is borderless so its frame can be a themed rounded rect
            // rather than the system's client edge. Drawn here, outside the
            // edit's own rect, so the edit repainting never erases it.
            RECT frame = ScaleRect(host, state->mask_field_rect);
            OffsetRect(&frame, 0, -state->host_scroll);
            FillRounded(
                dc,
                frame,
                ScaleForWindow(host, 4),
                state->palette.field,
                GetFocus() == state->ui_mask_edit ? state->palette.accent
                                                  : state->palette.field_line);
        }
        EndPaint(host, &paint);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
        return HandleControlColor(*state, message, wparam, lparam);
    case WM_MEASUREITEM: {
        auto* const measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
        if (measure && measure->CtlType == ODT_COMBOBOX) {
            measure->itemHeight = static_cast<UINT>(ScaleForWindow(host, 24));
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        const auto* const item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        if (item && item->CtlType == ODT_COMBOBOX) {
            DrawComboItem(*state, *item);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
    case WM_NOTIFY:
        // Every route the launcher already had stays on the launcher's window
        // procedure; the host only exists to clip and scroll.
        return SendMessageW(state->window, message, wparam, lparam);
    case WM_VSCROLL: {
        SCROLLINFO scroll{sizeof(scroll)};
        scroll.fMask = SIF_ALL;
        GetScrollInfo(host, SB_VERT, &scroll);
        const int line = ScaleForWindow(host, 32);
        int position = scroll.nPos;
        switch (LOWORD(wparam)) {
        case SB_LINEUP:
            position -= line;
            break;
        case SB_LINEDOWN:
            position += line;
            break;
        case SB_PAGEUP:
            position -= static_cast<int>(scroll.nPage);
            break;
        case SB_PAGEDOWN:
            position += static_cast<int>(scroll.nPage);
            break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
            position = scroll.nTrackPos;
            break;
        case SB_TOP:
            position = 0;
            break;
        case SB_BOTTOM:
            position = scroll.nMax;
            break;
        default:
            break;
        }
        ScrollHostTo(*state, position);
        return 0;
    }
    case WM_MOUSEWHEEL:
        ScrollHostTo(
            *state,
            state->host_scroll -
                GET_WHEEL_DELTA_WPARAM(wparam) * ScaleForWindow(host, 60) / WHEEL_DELTA);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(host, message, wparam, lparam);
}

bool RegisterSettingsHostClass(const HINSTANCE instance) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = SettingsHostProcedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = kSettingsHostClassName;
    return RegisterClassExW(&window_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

void DestroyLauncherFonts(LauncherState& state) {
    for (HFONT* const font : {&state.normal_font, &state.semibold_font, &state.small_font,
                              &state.caption_font, &state.heading_font}) {
        if (*font) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
}

LRESULT CALLBACK LauncherWindowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
    auto* state = reinterpret_cast<LauncherState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* const create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<LauncherState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
    case WM_CREATE:
        if (!state || !CreateLauncherControls(*state)) {
            return -1;
        }
        PopulateTargets(*state);
        WarmShaderCache();
        SetTimer(window, kProcessTimerId, 200, nullptr);
        return 0;
    case WM_ERASEBKGND:
        if (state) {
            RECT client{};
            GetClientRect(window, &client);
            FillFlat(reinterpret_cast<HDC>(wparam), client, state->palette.background);
            return 1;
        }
        break;
    case WM_PAINT:
        if (state) {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window, &paint);
            if (dc) {
                RECT client{};
                GetClientRect(window, &client);
                RECT band = client;
                band.top = client.bottom - ScaleForWindow(window, kFooterHeight);
                FillFlat(dc, band, state->palette.band);
                RECT edge = band;
                edge.bottom = edge.top + 1;
                FillFlat(dc, edge, state->palette.line);
            }
            EndPaint(window, &paint);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
        if (state) {
            return HandleControlColor(*state, message, wparam, lparam);
        }
        break;
    case WM_DRAWITEM: {
        const auto* const item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        if (state && item && item->CtlType == ODT_STATIC && item->CtlID == kStatusText) {
            DrawStatusPanel(*state, *item);
            return TRUE;
        }
        break;
    }
    case WM_NOTIFY:
        if (state && HandleTooltipNotify(*state, lparam)) {
            return 0;
        }
        break;
    case WM_SETTINGCHANGE:
        // "ImmersiveColorSet" is what a light/dark switch broadcasts.
        if (state && lparam &&
            CompareStringOrdinal(
                reinterpret_cast<const wchar_t*>(lparam), -1, L"ImmersiveColorSet", -1, TRUE) ==
                CSTR_EQUAL) {
            ApplyTheme(*state, osss::SystemPrefersDarkApps());
            return 0;
        }
        break;
    case WM_COMMAND:
        if (!state) {
            break;
        }
        {
            osss::TestGraphicsApi test_api{};
            int test_fps = 0;
            if (DecodeTestAnimationCommand(LOWORD(wparam), test_api, test_fps)) {
                LaunchTestAnimation(*state, test_api, test_fps);
                return 0;
            }
        }
        switch (LOWORD(wparam)) {
        case kAdvancedToggle:
            SetAdvancedExpanded(*state, !state->advanced_expanded, true);
            return 0;
        case kProfileSaveButton:
            SaveTargetProfile(*state);
            return 0;
        case kRefreshButton:
            PopulateTargets(*state);
            return 0;
        case kStartButton:
            BeginGeneration(*state);
            return 0;
        case kStopButton:
            RequestStop(*state);
            return 0;
        case kUiMaskEdit:
            if (HIWORD(wparam) == EN_CHANGE) {
                UpdateMaskHint(*state);
            }
            return 0;
        case kTargetCombo:
        case kTargetFpsCombo:
        case kMultiplierCombo:
        case kBufferCombo:
        case kCeilingPacingCombo:
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                UpdateControlAvailability(*state);
                UpdateReadyStatus(*state);
            }
            return 0;
        default:
            break;
        }
        break;
    case WM_TIMER:
        if (state && wparam == kProcessTimerId) {
            PollTestAnimation(*state);
            PollChild(*state);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (state && state->child_process) {
            state->closing = true;
            RequestStop(*state);
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kProcessTimerId);
        if (state) {
            CloseChildHandles(*state);
            FinishPendingTestLaunch(*state);
            DestroyLauncherFonts(*state);
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool HasArgument(const std::wstring_view expected) {
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) {
        return false;
    }
    bool found = false;
    for (int index = 1; index < count; ++index) {
        if (expected == arguments[index]) {
            found = true;
            break;
        }
    }
    LocalFree(arguments);
    return found;
}

bool RegisterLauncherClass(const HINSTANCE instance) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = LauncherWindowProcedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    window_class.hIconSm = window_class.hIcon;
    // Painted in WM_ERASEBKGND instead: the dark theme needs a fill
    // COLOR_BTNFACE cannot give it.
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = kLauncherClassName;
    return RegisterClassExW(&window_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HMENU CreateLauncherMenu() {
    const HMENU root = CreateMenu();
    const HMENU developer = CreatePopupMenu();
    const HMENU test_animations = CreatePopupMenu();
    if (!root || !developer || !test_animations) {
        if (test_animations) {
            DestroyMenu(test_animations);
        }
        if (developer) {
            DestroyMenu(developer);
        }
        if (root) {
            DestroyMenu(root);
        }
        return nullptr;
    }

    for (std::size_t api_index = 0; api_index < osss::kTestGraphicsApis.size(); ++api_index) {
        const HMENU api_menu = CreatePopupMenu();
        if (!api_menu) {
            DestroyMenu(root);
            DestroyMenu(developer);
            DestroyMenu(test_animations);
            return nullptr;
        }
        for (std::size_t rate_index = 0;
             rate_index < osss::kTestAnimationBaseRates.size();
             ++rate_index) {
            const int command = kTestAnimationMenuFirst + static_cast<int>(
                api_index * osss::kTestAnimationBaseRates.size() + rate_index);
            const std::wstring label =
                std::to_wstring(osss::kTestAnimationBaseRates[rate_index]) + L" FPS";
            AppendMenuW(api_menu, MF_STRING, command, label.c_str());
        }
        const auto api_name = osss::TestGraphicsApiDisplayName(osss::kTestGraphicsApis[api_index]);
        AppendMenuW(
            test_animations,
            MF_POPUP,
            reinterpret_cast<UINT_PTR>(api_menu),
            std::wstring(api_name).c_str());
    }
    AppendMenuW(
        developer,
        MF_POPUP,
        reinterpret_cast<UINT_PTR>(test_animations),
        L"Test animation");
    AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(developer), L"Dev");
    return root;
}



HWND CreateLauncherWindow(const HINSTANCE instance, LauncherState& state) {
    const UINT dpi = GetDpiForSystem();
    // A starting size only: WM_CREATE lays the controls out and ApplyGeometry
    // resizes to whatever that walk produced, clamped to the work area.
    RECT bounds{
        0,
        0,
        MulDiv(kWindowWidth, static_cast<int>(dpi), 96),
        MulDiv(558, static_cast<int>(dpi), 96),
    };
    AdjustWindowRectExForDpi(
        &bounds, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, TRUE, 0, dpi);
    const HMENU menu = CreateLauncherMenu();
    if (!menu) {
        return nullptr;
    }
    const HWND window = CreateWindowExW(
        0,
        kLauncherClassName,
        kLauncherTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        nullptr,
        menu,
        instance,
        &state);
    if (!window) {
        DestroyMenu(menu);
    }
    return window;
}

// Every control the layout walk placed, in the state named, in the coordinates
// its parent laid it out in. This is the check that would have caught the
// buffer-floor combo: four literals that happened to overlap, invisible to the
// compiler and to every other test in this repository.
bool PlacedRectsAreDisjoint(const LauncherState& state, const bool expanded, std::wstring& where) {
    for (std::size_t first = 0; first < state.placed.size(); ++first) {
        const PlacedControl& a = state.placed[first];
        if (a.advanced && !expanded) {
            continue;
        }
        for (std::size_t second = first + 1; second < state.placed.size(); ++second) {
            const PlacedControl& b = state.placed[second];
            if (b.advanced && !expanded) {
                continue;
            }
            if (a.parent != b.parent) {
                continue;
            }
            const bool overlaps = a.rect.left < b.rect.right && b.rect.left < a.rect.right &&
                a.rect.top < b.rect.bottom && b.rect.top < a.rect.bottom;
            if (overlaps) {
                where = L"controls " + std::to_wstring(first) + L" and " +
                    std::to_wstring(second);
                return false;
            }
        }
    }
    return true;
}

int RunLauncherSelfTest(const HINSTANCE instance) {
    LauncherState state;
    const HWND window = CreateLauncherWindow(instance, state);
    if (!window) {
        return 1;
    }
    const bool controls_exist =
        state.settings_host && state.tooltip && state.target_combo && state.refresh_button &&
        state.target_fps_combo && state.max_multiplier_combo && state.buffer_combo &&
        state.ceiling_pacing_combo && state.present_mode_combo && state.pacing_mode_combo &&
        state.flow_scale_combo && state.performance_mode_checkbox &&
        state.temporal_prior_checkbox && state.output_mode_combo && state.upscale_combo &&
        state.debug_view_combo && state.profile_checkbox && state.profile_save_button &&
        state.interpolator_combo && state.ui_mask_edit && state.ui_mask_hint &&
        state.ui_mask_auto_checkbox && state.stats_checkbox && state.advanced_toggle &&
        state.footer_hint && state.start_button && state.stop_button && state.status_text;
    const bool ui_mask_defaults_empty = UiMaskText(state).empty() &&
        SendMessageW(state.ui_mask_auto_checkbox, BM_GETCHECK, 0, 0) == BST_UNCHECKED;
    SetWindowTextW(state.ui_mask_edit, L"0,0,0.25,0.2; 1600,980,1920,1080px");
    const auto ui_mask_round_trip = osss::ParseUiMaskRects(UiMaskText(state));
    const bool ui_mask_edit_is_readable =
        ui_mask_round_trip.Ok() && ui_mask_round_trip.rects.size() == 2 &&
        !ui_mask_round_trip.rects[0].pixels && ui_mask_round_trip.rects[1].pixels;
    // The live parse feedback is part of the redesign's contract with the user:
    // a typo is reported while it is being typed, not on Start.
    UpdateMaskHint(state);
    const bool mask_hint_counts_regions = WindowText(state.ui_mask_hint) == L"2 regions parsed.";
    SetWindowTextW(state.ui_mask_edit, L"nonsense");
    UpdateMaskHint(state);
    const bool mask_hint_reports_errors = state.status_level == StatusLevel::warning &&
        TextRoleOf(state.ui_mask_hint) == TextRole::warning;
    SetWindowTextW(state.ui_mask_edit, L"");
    UpdateMaskHint(state);
    const bool settings_are_complete =
        SendMessageW(state.target_fps_combo, CB_GETCOUNT, 0, 0) ==
            static_cast<LRESULT>(kTargetFpsOptions.size() + 1) &&
        SendMessageW(state.target_fps_combo, CB_GETCURSEL, 0, 0) == 0 &&
        SendMessageW(state.max_multiplier_combo, CB_GETCOUNT, 0, 0) ==
            static_cast<LRESULT>(osss::kMaximumMultiplier - osss::kMinimumMultiplier + 1) &&
        SendMessageW(state.buffer_combo, CB_GETCOUNT, 0, 0) ==
            static_cast<LRESULT>(kBufferFloorOptions.size()) &&
        SendMessageW(state.interpolator_combo, CB_GETCOUNT, 0, 0) == 2 &&
        SendMessageW(state.flow_scale_combo, CB_GETCOUNT, 0, 0) == 4 &&
        SendMessageW(state.flow_scale_combo, CB_GETCURSEL, 0, 0) == 0 &&
        SelectedFlowScale(state) == osss::FlowScale::automatic &&
        SendMessageW(state.performance_mode_checkbox, BM_GETCHECK, 0, 0) == BST_UNCHECKED &&
        SendMessageW(state.temporal_prior_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED &&
        SendMessageW(state.output_mode_combo, CB_GETCOUNT, 0, 0) == 2 &&
        SelectedOutputMode(state) == osss::OutputMode::overlay &&
        SendMessageW(state.upscale_combo, CB_GETCOUNT, 0, 0) == 3 &&
        SelectedUpscaleMode(state) == osss::UpscaleMode::automatic &&
        SendMessageW(state.debug_view_combo, CB_GETCOUNT, 0, 0) == 4 &&
        SelectedDebugView(state) == osss::DebugView::off &&
        SendMessageW(state.profile_checkbox, BM_GETCHECK, 0, 0) == BST_UNCHECKED &&
        state.profile_save_button != nullptr &&
        SendMessageW(state.max_multiplier_combo, CB_GETCURSEL, 0, 0) == 4 &&
        SendMessageW(state.buffer_combo, CB_GETCURSEL, 0, 0) == 2 &&
        SelectedBufferMilliseconds(state) == osss::FrameSelector::kDefaultBufferMilliseconds &&
        SendMessageW(state.ceiling_pacing_combo, CB_GETCOUNT, 0, 0) == 2 &&
        SendMessageW(state.ceiling_pacing_combo, CB_GETCURSEL, 0, 0) == 0 &&
        SelectedCeilingPacing(state) == osss::FrameSelector::CeilingPacing::even &&
        SendMessageW(state.present_mode_combo, CB_GETCOUNT, 0, 0) == 3 &&
        SendMessageW(state.present_mode_combo, CB_GETCURSEL, 0, 0) == 0 &&
        SelectedPresentMode(state) == osss::PresentMode::automatic &&
        SendMessageW(state.pacing_mode_combo, CB_GETCOUNT, 0, 0) == 3 &&
        SendMessageW(state.pacing_mode_combo, CB_GETCURSEL, 0, 0) == 0 &&
        SelectedPacingMode(state) == osss::PacingMode::paced &&
        SendMessageW(state.interpolator_combo, CB_GETCURSEL, 0, 0) == 0 &&
        SendMessageW(state.stats_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
    // Every present-mode item must map to the argument osss.exe parses, in the
    // order SelectedPresentMode assumes. A combo reordered without updating that
    // switch would otherwise silently launch sessions in the wrong mode.
    bool present_modes_map_to_arguments = true;
    {
        const std::pair<int, const wchar_t*> expected[] = {
            {0, L"auto"},
            {1, L"tearing"},
            {2, L"vsync"},
        };
        for (const auto& [index, argument] : expected) {
            SendMessageW(state.present_mode_combo, CB_SETCURSEL, index, 0);
            if (std::wstring(osss::PresentModeArgument(SelectedPresentMode(state))) != argument) {
                present_modes_map_to_arguments = false;
            }
        }
        SendMessageW(state.present_mode_combo, CB_SETCURSEL, 0, 0);
    }
    // Same guard for the pacing combo, in the order SelectedPacingMode assumes.
    bool pacing_modes_map_to_arguments = true;
    {
        const std::pair<int, const wchar_t*> expected[] = {
            {0, L"paced"},
            {1, L"queued"},
            {2, L"unpaced"},
        };
        for (const auto& [index, argument] : expected) {
            SendMessageW(state.pacing_mode_combo, CB_SETCURSEL, index, 0);
            if (std::wstring(osss::PacingModeArgument(SelectedPacingMode(state))) != argument ||
                !osss::ParsePacingMode(argument).has_value()) {
                pacing_modes_map_to_arguments = false;
            }
        }
        SendMessageW(state.pacing_mode_combo, CB_SETCURSEL, 0, 0);
    }
    // ProfileArgumentsFor promises to stay in step with the launch command
    // line; the cheapest honest check is that every flag it emits is one
    // osss.exe --help documents. The launch line above is built from the same
    // Selected* accessors, so this also catches a control that has an accessor
    // but was never wired to either.
    const bool profile_flags_are_known = [&]() {
        static constexpr const wchar_t* known[] = {
            L"--target-fps", L"--max-multiplier", L"--buffer", L"--ceiling-pacing",
            L"--present-mode", L"--pacing", L"--flow-scale", L"--performance-mode",
            L"--temporal-prior",
            L"--interpolator", L"--output-mode", L"--upscale", L"--debug-view",
            L"--stats-overlay", L"--ui-mask", L"--ui-mask-auto",
        };
        const auto arguments = ProfileArgumentsFor(state);
        for (std::size_t index = 0; index + 1 < arguments.size(); index += 2) {
            bool found = false;
            for (const wchar_t* flag : known) {
                if (arguments[index] == flag) {
                    found = true;
                }
            }
            if (!found) {
                return false;
            }
        }
        return true;
    }();
    // Every entry in kTips reaches at least one control. A tooltip written but
    // never registered is the failure mode this redesign is most exposed to,
    // because nothing else in the build refers to the table.
    const bool every_tip_is_registered = [&]() {
        std::array<bool, static_cast<std::size_t>(Tip::count)> seen{};
        for (const TooltipEntry& entry : state.tooltips) {
            seen[static_cast<std::size_t>(entry.tip)] = true;
        }
        for (const bool present : seen) {
            if (!present) {
                return false;
            }
        }
        return true;
    }();
    const bool tips_cover_every_setting =
        state.tooltips.size() >= static_cast<std::size_t>(Tip::count);
    std::wstring overlap;
    const bool collapsed_layout_is_disjoint = PlacedRectsAreDisjoint(state, false, overlap);
    const bool expanded_layout_is_disjoint = PlacedRectsAreDisjoint(state, true, overlap);
    // Both states fit a 1080-high work area once the clamp is applied, and the
    // expanded state that does not fit is reachable by scrolling rather than
    // cut off.
    const int collapsed_wanted = WantedClientHeight(state, false);
    const int expanded_wanted = WantedClientHeight(state, true);
    const bool both_heights_fit_1080 =
        ClampClientHeight(collapsed_wanted, 1080) <= 1080 - kWorkAreaReserve &&
        ClampClientHeight(expanded_wanted, 1080) <= 1080 - kWorkAreaReserve &&
        collapsed_wanted <= 1080 - kWorkAreaReserve &&
        expanded_wanted > collapsed_wanted;
    // The disclosure really hides and shows the twelve-plus advanced controls.
    const bool advanced_starts_hidden_when_collapsed = [&]() {
        // The style bit, not IsWindowVisible: the self-test never shows the
        // launcher window, so nothing under it is visible on screen either way.
        const auto shown = [](const HWND control) {
            return (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0;
        };
        SetAdvancedExpanded(state, false, false);
        for (const HWND control : state.advanced_controls) {
            if (shown(control)) {
                return false;
            }
        }
        SetAdvancedExpanded(state, true, false);
        for (const HWND control : state.advanced_controls) {
            if (!shown(control)) {
                return false;
            }
        }
        SetAdvancedExpanded(state, false, false);
        return !state.advanced_controls.empty();
    }();
    const bool dynamic_ceiling_is_selectable =
        SendMessageW(state.max_multiplier_combo, CB_SETCURSEL, 0, 0) != CB_ERR &&
        SelectedMaxMultiplier(state) == 2;
    SendMessageW(state.max_multiplier_combo, CB_SETCURSEL, 4, 0);
    const bool manual_240_target_is_available =
        SendMessageW(state.target_fps_combo, CB_GETITEMDATA, 8, 0) == 240;
    const std::wstring cli_path = ExecutablePath(L"osss.exe");
    const bool cli_exists =
        !cli_path.empty() && GetFileAttributesW(cli_path.c_str()) != INVALID_FILE_ATTRIBUTES;
    const std::wstring test_animation_path = ExecutablePath(L"osss_test_animation.exe");
    const bool test_animation_exists =
        !test_animation_path.empty() &&
        GetFileAttributesW(test_animation_path.c_str()) != INVALID_FILE_ATTRIBUTES;
    const HMENU menu = GetMenu(window);
    const HMENU developer_menu = menu ? GetSubMenu(menu, 0) : nullptr;
    const HMENU test_animation_menu = developer_menu ? GetSubMenu(developer_menu, 0) : nullptr;
    bool rate_submenus_are_complete = test_animation_menu != nullptr;
    if (test_animation_menu) {
        for (int api_index = 0;
             api_index < static_cast<int>(osss::kTestGraphicsApis.size());
             ++api_index) {
            const HMENU rate_menu = GetSubMenu(test_animation_menu, api_index);
            rate_submenus_are_complete = rate_submenus_are_complete && rate_menu &&
                GetMenuItemCount(rate_menu) ==
                    static_cast<int>(osss::kTestAnimationBaseRates.size());
        }
    }
    const bool test_matrix_is_complete =
        menu && GetMenuItemCount(menu) == 1 && developer_menu &&
        GetMenuItemCount(developer_menu) == 1 && test_animation_menu &&
        GetMenuItemCount(test_animation_menu) ==
            static_cast<int>(osss::kTestGraphicsApis.size()) &&
        rate_submenus_are_complete;
    osss::TestGraphicsApi decoded_api{};
    int decoded_fps = 0;
    const bool last_test_command_is_addressable =
        DecodeTestAnimationCommand(kTestAnimationMenuLast, decoded_api, decoded_fps) &&
        decoded_api == osss::TestGraphicsApi::direct3d12 && decoded_fps == 120;
    const bool dpi_is_correct = osss::IsPerMonitorV2DpiAware();
    DestroyWindow(window);
    return controls_exist && ui_mask_defaults_empty && ui_mask_edit_is_readable &&
        mask_hint_counts_regions && mask_hint_reports_errors &&
        settings_are_complete && present_modes_map_to_arguments &&
        pacing_modes_map_to_arguments && profile_flags_are_known &&
        every_tip_is_registered && tips_cover_every_setting &&
        collapsed_layout_is_disjoint && expanded_layout_is_disjoint &&
        both_heights_fit_1080 && advanced_starts_hidden_when_collapsed &&
        dynamic_ceiling_is_selectable &&
        manual_240_target_is_available && cli_exists && test_animation_exists &&
        test_matrix_is_complete && last_test_command_is_addressable && dpi_is_correct ? 0 : 1;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    if (!osss::EnablePerMonitorV2DpiAwareness()) {
        return 1;
    }
    INITCOMMONCONTROLSEX common_controls{
        sizeof(common_controls),
        ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES,
    };
    InitCommonControlsEx(&common_controls);
    if (!RegisterLauncherClass(instance) || !RegisterSettingsHostClass(instance)) {
        return 1;
    }
    if (HasArgument(L"--self-test")) {
        return RunLauncherSelfTest(instance);
    }

    LauncherState state;
    const HWND window = CreateLauncherWindow(instance, state);
    if (!window) {
        return 1;
    }
    ShowWindow(window, show_command == 0 ? SW_SHOWNORMAL : show_command);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return static_cast<int>(message.wParam);
}
