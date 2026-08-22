#include "ui_mask.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace osss {
namespace {

std::string Narrow(const std::wstring_view text) {
    std::string result;
    result.reserve(text.size());
    for (const wchar_t character : text) {
        result.push_back(character < 0x80 ? static_cast<char>(character) : '?');
    }
    return result;
}

std::wstring Widen(const std::string_view text) {
    return std::wstring(text.begin(), text.end());
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

bool ParseNumber(const std::string_view text, double& value) {
    const std::string_view trimmed = Trim(text);
    if (trimmed.empty()) {
        return false;
    }
    for (const char character : trimmed) {
        if (!(std::isdigit(static_cast<unsigned char>(character)) || character == '.' ||
              character == '+' || character == '-')) {
            return false;
        }
    }
    const std::string owned(trimmed);
    char* end = nullptr;
    const double parsed = std::strtod(owned.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

std::string RegionError(const std::string_view region, const char* reason) {
    std::string message = "UI mask region \"";
    message.append(Trim(region));
    message.append("\": ");
    message.append(reason);
    return message;
}

} // namespace

UiMaskParseResult ParseUiMaskRects(const std::wstring_view text) {
    return ParseUiMaskRects(std::string_view(Narrow(text)));
}

UiMaskParseResult ParseUiMaskRects(const std::string_view text) {
    UiMaskParseResult result;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find_first_of(";\r\n", start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view region = text.substr(start, end - start);
        start = end + 1;

        std::string_view body = Trim(region);
        if (body.empty()) {
            continue;
        }

        bool pixels = false;
        if (body.size() >= 2) {
            const char suffix_a = static_cast<char>(std::tolower(static_cast<unsigned char>(body[body.size() - 2])));
            const char suffix_b = static_cast<char>(std::tolower(static_cast<unsigned char>(body[body.size() - 1])));
            if (suffix_a == 'p' && suffix_b == 'x') {
                pixels = true;
                body = Trim(body.substr(0, body.size() - 2));
            }
        }

        std::array<double, 4> values{};
        std::size_t value_index = 0;
        std::size_t field_start = 0;
        bool malformed = false;
        while (field_start <= body.size()) {
            std::size_t field_end = body.find(',', field_start);
            if (field_end == std::string_view::npos) {
                field_end = body.size();
            }
            if (value_index >= values.size() ||
                !ParseNumber(body.substr(field_start, field_end - field_start), values[value_index])) {
                malformed = true;
                break;
            }
            ++value_index;
            field_start = field_end + 1;
        }
        if (malformed || value_index != values.size()) {
            result.error = RegionError(
                region,
                "expected four comma-separated numbers left,top,right,bottom "
                "(fractions 0-1, or pixels with a px suffix).");
            result.rects.clear();
            return result;
        }

        for (const double value : values) {
            if (value < 0.0) {
                result.error = RegionError(region, "coordinates must not be negative.");
                result.rects.clear();
                return result;
            }
        }
        if (!pixels) {
            const bool any_above_one = std::any_of(
                values.begin(),
                values.end(),
                [](const double value) { return value > 1.0; });
            pixels = any_above_one;
        }
        if (values[2] <= values[0] || values[3] <= values[1]) {
            result.error = RegionError(region, "right must exceed left and bottom must exceed top.");
            result.rects.clear();
            return result;
        }

        UiMaskRect rect;
        rect.left = values[0];
        rect.top = values[1];
        rect.right = values[2];
        rect.bottom = values[3];
        rect.pixels = pixels;
        result.rects.push_back(rect);
    }
    return result;
}

UiMaskPixelRect ResolveUiMaskRect(
    const UiMaskRect& rect,
    const std::uint32_t source_width,
    const std::uint32_t source_height) noexcept {
    const auto resolve = [](const double value, const bool pixels, const std::uint32_t extent, const bool round_up) {
        const double scaled = pixels ? value : value * static_cast<double>(extent);
        const double clamped = std::clamp(scaled, 0.0, static_cast<double>(extent));
        const double rounded = round_up ? std::ceil(clamped - 1e-6) : std::floor(clamped + 1e-6);
        return static_cast<std::uint32_t>(std::clamp(rounded, 0.0, static_cast<double>(extent)));
    };
    UiMaskPixelRect resolved;
    resolved.left = resolve(rect.left, rect.pixels, source_width, false);
    resolved.top = resolve(rect.top, rect.pixels, source_height, false);
    resolved.right = resolve(rect.right, rect.pixels, source_width, true);
    resolved.bottom = resolve(rect.bottom, rect.pixels, source_height, true);
    return resolved;
}

std::vector<std::uint8_t> RasterizeUiMask(
    const std::vector<UiMaskRect>& rects,
    const std::uint32_t source_width,
    const std::uint32_t source_height) {
    std::vector<std::uint8_t> coverage(
        static_cast<std::size_t>(source_width) * static_cast<std::size_t>(source_height),
        0);
    for (const UiMaskRect& rect : rects) {
        const UiMaskPixelRect bounds = ResolveUiMaskRect(rect, source_width, source_height);
        if (bounds.Empty()) {
            continue;
        }
        for (std::uint32_t y = bounds.top; y < bounds.bottom; ++y) {
            std::uint8_t* row = coverage.data() + static_cast<std::size_t>(y) * source_width;
            std::fill(row + bounds.left, row + bounds.right, static_cast<std::uint8_t>(255));
        }
    }
    return coverage;
}

std::wstring FormatUiMaskRects(const std::vector<UiMaskRect>& rects) {
    std::ostringstream stream;
    bool first = true;
    for (const UiMaskRect& rect : rects) {
        if (!first) {
            stream << "; ";
        }
        first = false;
        if (rect.pixels) {
            stream
                << static_cast<long long>(std::llround(rect.left)) << ','
                << static_cast<long long>(std::llround(rect.top)) << ','
                << static_cast<long long>(std::llround(rect.right)) << ','
                << static_cast<long long>(std::llround(rect.bottom)) << "px";
        } else {
            stream << std::setprecision(4)
                << rect.left << ',' << rect.top << ',' << rect.right << ',' << rect.bottom;
        }
    }
    return Widen(stream.str());
}

} // namespace osss
