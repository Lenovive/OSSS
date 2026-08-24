#pragma once

#include "platform/int_rect.h"
#include "platform/rgb_color.h"

namespace osss {

// The launcher's colour tokens for one theme.
//
// Held as a struct that every paint routine takes by reference, rather than as
// free functions keyed on a bool: the theme is resolved once per repaint and
// every colour in that repaint comes from the same set, so a routine cannot
// read the light field colour onto a dark background halfway down the window.
struct LauncherPalette {
    RgbColor background;
    RgbColor surface;
    RgbColor foreground;
    RgbColor foreground_muted;
    RgbColor foreground_faint;
    RgbColor line;
    RgbColor field;
    RgbColor field_line;
    RgbColor band;
    RgbColor accent;
    RgbColor accent_foreground;
    RgbColor tooltip_background;
    RgbColor tooltip_foreground;
    RgbColor tooltip_line;
    // Status-panel state colours. These are the only three colours in the
    // launcher that carry meaning rather than structure.
    RgbColor ok;
    RgbColor warning;
    RgbColor failure;
};

[[nodiscard]] LauncherPalette LauncherPaletteFor(bool dark) noexcept;

// On Windows this reads HKCU\Software\Microsoft\Windows\CurrentVersion\Themes
// \Personalize\AppsUseLightTheme; a missing or unreadable value is light, which
// is what Windows itself falls back to. Other platforms have no equivalent
// switch and report light. There is deliberately no in-app switch: the
// launcher follows the system and nothing else.
[[nodiscard]] bool SystemPrefersDarkApps() noexcept;

// Mixes a colour toward white (positive percent) or black (negative), for hover
// and pressed states that have no token of their own.
[[nodiscard]] RgbColor ShadeColor(RgbColor color, int percent) noexcept;

// Advances a y cursor and hands out control rects, in logical pixels at 96 DPI
// -- the same units the launcher's CreateControl scales by the window DPI.
//
// Every position in CreateLauncherControls used to be an independent literal.
// Inserting a row silently overlapped its neighbour, and that was not
// hypothetical: the adaptive buffer-floor combo spent revisions painted over by
// the present-mode combo, surviving as an 8 px sliver of its left edge, because
// nothing in the build or the self-test could see it. Asking the cursor for a
// rect makes that class of bug unwritable, and makes both window heights --
// collapsed and expanded -- fall out of the same walk that creates the
// controls, instead of being two more literals to keep in step.
class LauncherLayout {
public:
    static constexpr int kContentLeft = 24;
    static constexpr int kContentWidth = 672;
    static constexpr int kContentRight = kContentLeft + kContentWidth;
    static constexpr int kGutter = 20;
    static constexpr int kLabelHeight = 19;
    // Gap between a field label and the control it names.
    static constexpr int kLabelGap = 3;
    static constexpr int kControlHeight = 32;
    static constexpr int kCheckboxHeight = 24;
    static constexpr int kCheckboxGap = 8;
    static constexpr int kCaptionHeight = 18;
    // Gap below a section caption.
    static constexpr int kCaptionGap = 6;
    // Gap between two rows inside one section.
    static constexpr int kRowGap = 16;
    // Gap between the last row of a section and the next caption.
    static constexpr int kSectionGap = 22;

    explicit LauncherLayout(const int top) noexcept : cursor_(top) {}

    [[nodiscard]] int Cursor() const noexcept {
        return cursor_;
    }

    void Gap(const int pixels) noexcept {
        cursor_ += pixels;
    }

    // A full-width row. Advances the cursor past it, because a full-width row
    // can have nothing beside it.
    [[nodiscard]] IntRect Row(int height) noexcept;

    // One column of an `of`-column row at the current cursor. Deliberately does
    // not advance: the columns of a row share a y, so the caller ends the row
    // with Gap(height). Column widths absorb the division remainder from the
    // right, so the last column's right edge always lands on kContentRight.
    [[nodiscard]] IntRect Column(int index, int of, int height) const noexcept;

private:
    int cursor_ = 0;
};

} // namespace osss
