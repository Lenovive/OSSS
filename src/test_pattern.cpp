#include "test_pattern.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numbers>
#include <sstream>
#include <stdexcept>

namespace osss {
namespace {

struct Rgb {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
};

constexpr std::uint32_t Pack(const int red, const int green, const int blue) noexcept {
    return 0xFF000000U |
        (static_cast<std::uint32_t>(std::clamp(red, 0, 255)) << 16U) |
        (static_cast<std::uint32_t>(std::clamp(green, 0, 255)) << 8U) |
        static_cast<std::uint32_t>(std::clamp(blue, 0, 255));
}

Rgb Unpack(const std::uint32_t color) noexcept {
    return Rgb{
        static_cast<double>((color >> 16U) & 0xFFU),
        static_cast<double>((color >> 8U) & 0xFFU),
        static_cast<double>(color & 0xFFU),
    };
}

std::uint32_t Mix(
    const std::uint32_t background,
    const std::uint32_t foreground,
    const double coverage) noexcept {
    const double amount = std::clamp(coverage, 0.0, 1.0);
    const Rgb back = Unpack(background);
    const Rgb front = Unpack(foreground);
    return Pack(
        static_cast<int>(std::lround(back.red + (front.red - back.red) * amount)),
        static_cast<int>(std::lround(back.green + (front.green - back.green) * amount)),
        static_cast<int>(std::lround(back.blue + (front.blue - back.blue) * amount)));
}

class Canvas {
public:
    Canvas(const std::uint32_t width, const std::uint32_t height, const std::uint32_t background)
        : width_(width), height_(height), pixels_(static_cast<std::size_t>(width) * height, background) {
    }

    void Pixel(const int x, const int y, const std::uint32_t color, const double coverage = 1.0) {
        if (x < 0 || y < 0 || x >= static_cast<int>(width_) || y >= static_cast<int>(height_)) {
            return;
        }
        auto& destination = pixels_[static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x)];
        destination = Mix(destination, color, coverage);
    }

    void Rectangle(
        const int left,
        const int top,
        const int right,
        const int bottom,
        const std::uint32_t color) {
        const int clipped_left = std::clamp(left, 0, static_cast<int>(width_));
        const int clipped_top = std::clamp(top, 0, static_cast<int>(height_));
        const int clipped_right = std::clamp(right, 0, static_cast<int>(width_));
        const int clipped_bottom = std::clamp(bottom, 0, static_cast<int>(height_));
        for (int y = clipped_top; y < clipped_bottom; ++y) {
            auto* row = pixels_.data() + static_cast<std::size_t>(y) * width_;
            std::fill(row + clipped_left, row + clipped_right, color);
        }
    }

    void FractionalRectangle(
        const double left,
        const double top,
        const double right,
        const double bottom,
        const std::uint32_t color) {
        const int first_x = static_cast<int>(std::floor(left));
        const int last_x = static_cast<int>(std::ceil(right));
        const int first_y = static_cast<int>(std::floor(top));
        const int last_y = static_cast<int>(std::ceil(bottom));
        for (int y = first_y; y < last_y; ++y) {
            const double vertical = std::max(
                0.0,
                std::min(bottom, static_cast<double>(y + 1)) -
                    std::max(top, static_cast<double>(y)));
            for (int x = first_x; x < last_x; ++x) {
                const double horizontal = std::max(
                    0.0,
                    std::min(right, static_cast<double>(x + 1)) -
                        std::max(left, static_cast<double>(x)));
                Pixel(x, y, color, horizontal * vertical);
            }
        }
    }

    void Outline(
        const int left,
        const int top,
        const int right,
        const int bottom,
        const int thickness,
        const std::uint32_t color) {
        Rectangle(left, top, right, top + thickness, color);
        Rectangle(left, bottom - thickness, right, bottom, color);
        Rectangle(left, top, left + thickness, bottom, color);
        Rectangle(right - thickness, top, right, bottom, color);
    }

    [[nodiscard]] std::vector<std::uint32_t> TakePixels() {
        return std::move(pixels_);
    }

private:
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::vector<std::uint32_t> pixels_;
};

double WrappedSeconds(const double seconds) noexcept {
    double wrapped = std::fmod(seconds, TestPatternSpec::kCycleSeconds);
    if (wrapped < 0.0) {
        wrapped += TestPatternSpec::kCycleSeconds;
    }
    return wrapped;
}

std::size_t ApiIndex(const TestGraphicsApi api) noexcept {
    for (std::size_t index = 0; index < kTestGraphicsApis.size(); ++index) {
        if (kTestGraphicsApis[index] == api) {
            return index;
        }
    }
    return 0;
}

std::size_t RateIndex(const int fps) noexcept {
    for (std::size_t index = 0; index < kTestAnimationBaseRates.size(); ++index) {
        if (kTestAnimationBaseRates[index] == fps) {
            return index;
        }
    }
    return 0;
}

void DrawMachineHeader(
    Canvas& canvas,
    const TestPatternSpec& specification,
    const double phase,
    const std::uint64_t source_frame_index,
    const bool alternate_scene) {
    const std::uint32_t dark = alternate_scene ? Pack(30, 24, 17) : Pack(7, 11, 20);
    const std::uint32_t quiet = alternate_scene ? Pack(91, 73, 49) : Pack(35, 48, 69);
    const std::uint32_t bright = alternate_scene ? Pack(255, 221, 120) : Pack(116, 238, 255);
    const std::uint32_t hot = alternate_scene ? Pack(10, 161, 116) : Pack(255, 91, 146);
    canvas.Rectangle(
        0,
        0,
        static_cast<int>(specification.width),
        static_cast<int>(TestPatternSpec::kMachineHeaderHeight),
        dark);

    for (std::size_t index = 0; index < kTestGraphicsApis.size(); ++index) {
        const int left = 18 + static_cast<int>(index) * 31;
        canvas.Rectangle(left, 14, left + 22, 36, index == ApiIndex(specification.api) ? bright : quiet);
    }

    for (std::size_t index = 0; index < kTestAnimationBaseRates.size(); ++index) {
        const int left = 168 + static_cast<int>(index) * 25;
        const int height = index == RateIndex(specification.base_fps) ? 30 : 12;
        canvas.Rectangle(left, 45 - height, left + 13, 45, index == RateIndex(specification.base_fps) ? hot : quiet);
    }

    for (int bit = 0; bit < 20; ++bit) {
        const int left = 500 + bit * 18;
        const bool set = ((source_frame_index >> bit) & 1ULL) != 0;
        canvas.Rectangle(left, 15, left + 12, 43, set ? bright : quiet);
    }

    const double sweep = phase / TestPatternSpec::kCycleSeconds;
    const double sweep_x = 18.0 + sweep * (static_cast<double>(specification.width) - 36.0);
    canvas.FractionalRectangle(sweep_x - 1.5, 53.0, sweep_x + 1.5, 61.0, hot);
    if (phase < 0.20) {
        canvas.Outline(2, 2, static_cast<int>(specification.width) - 2, 62, 3, Pack(255, 255, 255));
    }
}

void DrawGrid(
    Canvas& canvas,
    const int left,
    const int top,
    const int right,
    const int bottom,
    const std::uint32_t line) {
    for (int x = left + 16; x < right; x += 32) {
        canvas.Rectangle(x, top, x + 1, bottom, line);
    }
    for (int y = top + 16; y < bottom; y += 32) {
        canvas.Rectangle(left, y, right, y + 1, line);
    }
}

void DrawLinearLane(
    Canvas& canvas,
    const TestPatternSpec& specification,
    const double animation_seconds,
    const bool alternate_scene) {
    constexpr int top = 78;
    constexpr int bottom = 206;
    const int right = static_cast<int>(specification.width) - 18;
    const std::uint32_t panel = alternate_scene ? Pack(226, 212, 176) : Pack(13, 24, 39);
    const std::uint32_t grid = alternate_scene ? Pack(187, 162, 113) : Pack(30, 57, 76);
    const std::uint32_t cyan = alternate_scene ? Pack(30, 72, 91) : Pack(71, 232, 255);
    const std::uint32_t magenta = alternate_scene ? Pack(153, 27, 70) : Pack(255, 67, 151);
    canvas.Rectangle(18, top, right, bottom, panel);
    DrawGrid(canvas, 18, top, right, bottom, grid);
    canvas.Outline(18, top, right, bottom, 2, grid);

    const double travel = static_cast<double>(right - 18 + 90);
    const double x = 18.0 - 72.0 + std::fmod(animation_seconds * 173.0, travel);
    const double y = 104.0 + 14.0 * std::sin(animation_seconds * 2.0 * std::numbers::pi / 2.0);
    canvas.FractionalRectangle(x, y, x + 72.0, y + 72.0, cyan);
    canvas.FractionalRectangle(x + 31.25, y - 10.0, x + 34.25, y + 82.0, magenta);

    for (int marker = 0; marker < 9; ++marker) {
        const int marker_x = 70 + marker * 100;
        canvas.Rectangle(marker_x, 187, marker_x + 2, 199, magenta);
    }
}

void DrawOcclusionLane(
    Canvas& canvas,
    const TestPatternSpec& specification,
    const double animation_seconds,
    const bool alternate_scene) {
    constexpr int top = 222;
    constexpr int bottom = 350;
    const int right = static_cast<int>(specification.width) - 18;
    const std::uint32_t first = alternate_scene ? Pack(61, 104, 79) : Pack(34, 31, 67);
    const std::uint32_t second = alternate_scene ? Pack(30, 59, 47) : Pack(18, 15, 42);
    const std::uint32_t moving_first = alternate_scene ? Pack(255, 130, 53) : Pack(254, 213, 73);
    const std::uint32_t moving_second = alternate_scene ? Pack(119, 42, 22) : Pack(255, 91, 146);
    const std::uint32_t occluder = alternate_scene ? Pack(14, 29, 24) : Pack(7, 11, 20);
    for (int y = top; y < bottom; y += 16) {
        for (int x = 18; x < right; x += 16) {
            canvas.Rectangle(x, y, std::min(x + 16, right), std::min(y + 16, bottom),
                (((x - 18) / 16 + (y - top) / 16) & 1) == 0 ? first : second);
        }
    }

    const double travel = static_cast<double>(right - 18 + 180);
    const double x = static_cast<double>(right) + 20.0 - std::fmod(animation_seconds * 227.0, travel);
    for (int stripe = 0; stripe < 6; ++stripe) {
        const double stripe_left = x + stripe * 22.0;
        canvas.FractionalRectangle(
            stripe_left,
            245.5,
            stripe_left + 18.0,
            326.5,
            (stripe & 1) == 0 ? moving_first : moving_second);
    }

    const int middle = static_cast<int>(specification.width) / 2;
    canvas.Rectangle(middle - 66, top - 2, middle + 66, bottom + 2, occluder);
    canvas.Rectangle(middle - 48, top + 18, middle + 48, bottom - 18, first);
    canvas.Outline(middle - 66, top - 2, middle + 66, bottom + 2, 4, moving_second);
}

void DrawDetailLane(
    Canvas& canvas,
    const TestPatternSpec& specification,
    const double animation_seconds,
    const bool alternate_scene) {
    constexpr int top = 366;
    constexpr int bottom = 522;
    const int right = static_cast<int>(specification.width) - 18;
    const std::uint32_t panel = alternate_scene ? Pack(221, 211, 185) : Pack(10, 17, 29);
    const std::uint32_t low = alternate_scene ? Pack(19, 50, 37) : Pack(24, 38, 62);
    const std::uint32_t high = alternate_scene ? Pack(21, 126, 88) : Pack(118, 255, 207);
    const std::uint32_t opposing = alternate_scene ? Pack(147, 30, 70) : Pack(255, 76, 145);
    canvas.Rectangle(18, top, right, bottom, panel);
    canvas.Outline(18, top, right, bottom, 2, low);

    const double shift = animation_seconds * 91.0;
    for (int y = top + 12; y < bottom - 12; ++y) {
        for (int x = 30; x < right - 12; ++x) {
            const double wave = 0.5 + 0.5 * std::sin(
                2.0 * std::numbers::pi * (static_cast<double>(x) - shift) / 6.0);
            canvas.Pixel(x, y, high, wave * 0.78);
        }
    }

    const double span = static_cast<double>(right - 54);
    const double forward = 30.0 + std::fmod(animation_seconds * 131.0, span);
    const double backward = static_cast<double>(right - 24) - std::fmod(animation_seconds * 157.0, span);
    canvas.FractionalRectangle(forward, top + 23.25, forward + 3.0, bottom - 23.25, opposing);
    canvas.FractionalRectangle(backward, top + 42.75, backward + 2.0, bottom - 42.75, high);

    const double ruler = 30.0 +
        (WrappedSeconds(animation_seconds) / TestPatternSpec::kCycleSeconds) *
            static_cast<double>(right - 60);
    canvas.FractionalRectangle(ruler - 2.0, bottom - 16.0, ruler + 2.0, bottom - 5.0, opposing);
}

// Static HUD panels: fixed position, content driven only by the source frame
// index. Cell 0 flips on every frame, so adjacent states differ at full
// contrast and a 50/50 blend lands far from both.
constexpr int kHudCellSize = 18;
constexpr int kHudCellGap = 4;
constexpr int kHudCellCount = 10;
constexpr int kHudPadding = 10;
constexpr int kHudPanelWidth = kHudPadding * 2 + kHudCellCount * kHudCellSize +
    (kHudCellCount - 1) * kHudCellGap;
constexpr int kHudPanelHeight = kHudPadding * 2 + kHudCellSize;

std::vector<TestPatternRect> HudRects(const TestPatternSpec& specification) {
    if (!specification.hud_overlay) {
        return {};
    }
    const int width = static_cast<int>(specification.width);
    const int height = static_cast<int>(specification.height);
    if (width < kHudPanelWidth + 60 || height < 540) {
        return {};
    }

    // Upper-left over the linear lane, and lower-right over the detail lane.
    const TestPatternRect upper_left{
        30U,
        124U,
        static_cast<std::uint32_t>(30 + kHudPanelWidth),
        static_cast<std::uint32_t>(124 + kHudPanelHeight),
    };
    const TestPatternRect lower_right{
        static_cast<std::uint32_t>(width - 30 - kHudPanelWidth),
        static_cast<std::uint32_t>(height - 40 - kHudPanelHeight),
        static_cast<std::uint32_t>(width - 30),
        static_cast<std::uint32_t>(height - 40),
    };
    return {upper_left, lower_right};
}

void DrawHudPanel(
    Canvas& canvas,
    const TestPatternRect& bounds,
    const std::uint64_t source_frame_index,
    const bool invert) {
    const std::uint32_t panel = Pack(6, 9, 14);
    const std::uint32_t border = Pack(150, 158, 172);
    const std::uint32_t set_color = invert ? Pack(255, 196, 64) : Pack(96, 245, 255);
    const std::uint32_t clear_color = Pack(24, 30, 40);

    canvas.Rectangle(
        static_cast<int>(bounds.left),
        static_cast<int>(bounds.top),
        static_cast<int>(bounds.right),
        static_cast<int>(bounds.bottom),
        panel);
    canvas.Outline(
        static_cast<int>(bounds.left),
        static_cast<int>(bounds.top),
        static_cast<int>(bounds.right),
        static_cast<int>(bounds.bottom),
        2,
        border);

    for (int cell = 0; cell < kHudCellCount; ++cell) {
        const int left = static_cast<int>(bounds.left) + kHudPadding +
            cell * (kHudCellSize + kHudCellGap);
        const int top = static_cast<int>(bounds.top) + kHudPadding;
        const bool set = ((source_frame_index >> static_cast<unsigned>(cell)) & 1ULL) != 0;
        canvas.Rectangle(
            left,
            top,
            left + kHudCellSize,
            top + kHudCellSize,
            set ? set_color : clear_color);
    }
}

void DrawHudOverlay(
    Canvas& canvas,
    const TestPatternSpec& specification,
    const std::uint64_t source_frame_index) {
    const auto rects = HudRects(specification);
    for (std::size_t index = 0; index < rects.size(); ++index) {
        DrawHudPanel(canvas, rects[index], source_frame_index, index == 1);
    }
}

std::string ReadPpmToken(std::istream& input) {
    std::string token;
    while (input >> token) {
        if (!token.empty() && token.front() == '#') {
            std::string ignored;
            std::getline(input, ignored);
            continue;
        }
        return token;
    }
    return {};
}

} // namespace

std::vector<std::uint32_t> RenderTestPattern(
    const TestPatternSpec& specification,
    const double animation_seconds,
    const std::uint64_t source_frame_index) {
    if (specification.width < 640 || specification.height < 480) {
        throw std::invalid_argument("The OSSS test pattern requires at least 640x480 pixels.");
    }
    if (!IsTestAnimationBaseRate(specification.base_fps)) {
        throw std::invalid_argument("Unsupported OSSS test-animation base rate.");
    }

    const double phase = WrappedSeconds(animation_seconds);
    const bool alternate_scene = phase >= TestPatternSpec::kCycleSeconds / 2.0;
    const std::uint32_t background = alternate_scene ? Pack(238, 229, 203) : Pack(4, 8, 15);
    Canvas canvas(specification.width, specification.height, background);
    DrawMachineHeader(canvas, specification, phase, source_frame_index, alternate_scene);
    DrawLinearLane(canvas, specification, animation_seconds, alternate_scene);
    DrawOcclusionLane(canvas, specification, animation_seconds, alternate_scene);
    DrawDetailLane(canvas, specification, animation_seconds, alternate_scene);
    DrawHudOverlay(canvas, specification, source_frame_index);
    return canvas.TakePixels();
}

std::vector<TestPatternRect> TestPatternHudRects(const TestPatternSpec& specification) {
    return HudRects(specification);
}

std::string TestPatternHudMaskArgument(const TestPatternSpec& specification) {
    std::ostringstream stream;
    bool first = true;
    for (const TestPatternRect& rect : HudRects(specification)) {
        if (!first) {
            stream << "; ";
        }
        first = false;
        stream << rect.left << ',' << rect.top << ',' << rect.right << ',' << rect.bottom << "px";
    }
    return stream.str();
}

double SourceFramePhaseMilliseconds(const double animation_seconds, const int base_fps) {
    if (base_fps <= 0) {
        return 0.0;
    }
    const double frames = animation_seconds * static_cast<double>(base_fps);
    const double nearest = std::round(frames);
    return (frames - nearest) / static_cast<double>(base_fps) * 1000.0;
}

bool IsSourceFrameInstant(
    const double animation_seconds,
    const int base_fps,
    const double tolerance_milliseconds) {
    return std::abs(SourceFramePhaseMilliseconds(animation_seconds, base_fps)) <=
        tolerance_milliseconds;
}

HudOverlayCheck CheckTestPatternHud(
    const std::span<const std::uint32_t> observed,
    const TestPatternSpec& specification,
    const std::uint64_t first_source_frame_index,
    const std::uint64_t last_source_frame_index,
    const std::uint8_t exact_match_tolerance) {
    HudOverlayCheck check;
    const auto rects = HudRects(specification);
    if (rects.empty() ||
        observed.size() !=
            static_cast<std::size_t>(specification.width) * specification.height) {
        return check;
    }

    const std::uint64_t last = std::max(first_source_frame_index, last_source_frame_index);
    for (std::uint64_t index = first_source_frame_index; index <= last; ++index) {
        // The HUD depends only on the source frame index, so any animation time
        // renders the same panels for a given index.
        const auto expected = RenderTestPattern(
            specification,
            static_cast<double>(index) / std::max(1, specification.base_fps),
            index);

        std::uint8_t worst = 0;
        std::uint64_t compared = 0;
        for (const TestPatternRect& rect : rects) {
            for (std::uint32_t y = rect.top; y < rect.bottom; ++y) {
                for (std::uint32_t x = rect.left; x < rect.right; ++x) {
                    const std::size_t pixel =
                        static_cast<std::size_t>(y) * specification.width + x;
                    for (const unsigned shift : {0U, 8U, 16U}) {
                        const int difference = std::abs(
                            static_cast<int>((expected[pixel] >> shift) & 0xFFU) -
                            static_cast<int>((observed[pixel] >> shift) & 0xFFU));
                        worst = std::max(worst, static_cast<std::uint8_t>(difference));
                    }
                    ++compared;
                }
            }
        }

        check.compared_pixels = compared;
        if (worst < check.maximum_channel_error) {
            check.maximum_channel_error = worst;
            check.matched_source_frame_index = index;
            check.matches_discrete_state = worst <= exact_match_tolerance;
        }
    }
    return check;
}

TestPatternMetrics CompareTestPatternFrames(
    const std::span<const std::uint32_t> expected,
    const std::span<const std::uint32_t> observed,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t scored_top,
    const std::uint8_t bad_pixel_threshold,
    const std::uint32_t sampling_stride,
    const std::span<const TestPatternRect> excluded_regions) {
    TestPatternMetrics metrics;
    if (width == 0 || height == 0 || expected.size() != observed.size() ||
        expected.size() != static_cast<std::size_t>(width) * height) {
        return metrics;
    }

    const auto excluded = [&](const std::uint32_t x, const std::uint32_t y) {
        for (const TestPatternRect& rect : excluded_regions) {
            if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom) {
                return true;
            }
        }
        return false;
    };

    const std::uint32_t stride = std::max(1U, sampling_stride);
    long double absolute_sum = 0.0;
    long double squared_sum = 0.0;
    std::uint64_t bad_pixels = 0;
    for (std::uint32_t y = std::min(scored_top, height); y < height; y += stride) {
        for (std::uint32_t x = 0; x < width; x += stride) {
            if (excluded(x, y)) {
                continue;
            }
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const std::uint32_t expected_pixel = expected[index];
            const std::uint32_t observed_pixel = observed[index];
            bool pixel_is_bad = false;
            for (const unsigned shift : {0U, 8U, 16U}) {
                const int difference = std::abs(
                    static_cast<int>((expected_pixel >> shift) & 0xFFU) -
                    static_cast<int>((observed_pixel >> shift) & 0xFFU));
                absolute_sum += difference;
                squared_sum += static_cast<long double>(difference) * difference;
                metrics.maximum_channel_error = std::max(
                    metrics.maximum_channel_error,
                    static_cast<std::uint8_t>(difference));
                pixel_is_bad = pixel_is_bad || difference > bad_pixel_threshold;
            }
            if (pixel_is_bad) {
                ++bad_pixels;
            }
            ++metrics.compared_pixels;
        }
    }

    if (metrics.compared_pixels == 0) {
        return metrics;
    }
    const long double channel_count = static_cast<long double>(metrics.compared_pixels) * 3.0L;
    metrics.mean_absolute_error = static_cast<double>(absolute_sum / channel_count);
    metrics.root_mean_square_error = static_cast<double>(std::sqrt(squared_sum / channel_count));
    if (metrics.root_mean_square_error > 0.0) {
        metrics.psnr_db = 20.0 * std::log10(255.0 / metrics.root_mean_square_error);
    }
    metrics.pixels_over_threshold_percent =
        100.0 * static_cast<double>(bad_pixels) / static_cast<double>(metrics.compared_pixels);
    return metrics;
}

bool WriteTestPatternPpm(
    const std::filesystem::path& path,
    const std::span<const std::uint32_t> pixels,
    const std::uint32_t width,
    const std::uint32_t height,
    std::string& error) {
    if (pixels.size() != static_cast<std::size_t>(width) * height) {
        error = "PPM pixel count does not match its dimensions.";
        return false;
    }
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        error = "Could not open the PPM output file.";
        return false;
    }
    output << "P6\n" << width << ' ' << height << "\n255\n";
    for (const std::uint32_t pixel : pixels) {
        const char bytes[3]{
            static_cast<char>((pixel >> 16U) & 0xFFU),
            static_cast<char>((pixel >> 8U) & 0xFFU),
            static_cast<char>(pixel & 0xFFU),
        };
        output.write(bytes, sizeof(bytes));
    }
    if (!output) {
        error = "Writing the PPM output file failed.";
        return false;
    }
    error.clear();
    return true;
}

bool ReadTestPatternPpm(
    const std::filesystem::path& path,
    std::vector<std::uint32_t>& pixels,
    std::uint32_t& width,
    std::uint32_t& height,
    std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Could not open the PPM input file.";
        return false;
    }
    if (ReadPpmToken(input) != "P6") {
        error = "Only binary P6 PPM images are supported.";
        return false;
    }
    try {
        width = static_cast<std::uint32_t>(std::stoul(ReadPpmToken(input)));
        height = static_cast<std::uint32_t>(std::stoul(ReadPpmToken(input)));
        if (ReadPpmToken(input) != "255" || width == 0 || height == 0) {
            error = "The PPM header is invalid.";
            return false;
        }
    } catch (const std::exception&) {
        error = "The PPM dimensions are invalid.";
        return false;
    }
    input.get();
    pixels.assign(static_cast<std::size_t>(width) * height, 0xFF000000U);
    for (auto& pixel : pixels) {
        unsigned char bytes[3]{};
        input.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
        if (!input) {
            error = "The PPM pixel data ended early.";
            pixels.clear();
            return false;
        }
        pixel = Pack(bytes[0], bytes[1], bytes[2]);
    }
    error.clear();
    return true;
}

} // namespace osss
