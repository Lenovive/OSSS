#include "platform/unicode.h"

#if defined(_WIN32)
#  include <windows.h>
#endif

#include <cstdint>
#include <stdexcept>

namespace osss {

#if defined(_WIN32)

std::string ToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (length <= 1) {
        throw std::runtime_error("WideCharToMultiByte failed.");
    }
    // length includes the NUL terminator, which the two-pass form writes into
    // the last byte; reserve one less and pass the full length as capacity.
    std::string output(static_cast<std::size_t>(length - 1), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        output.data(),
        length,
        nullptr,
        nullptr);
    if (written != length) {
        throw std::runtime_error("WideCharToMultiByte truncated its result.");
    }
    return output;
}

std::wstring ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 1) {
        throw std::runtime_error("MultiByteToWideChar failed.");
    }
    std::wstring output(static_cast<std::size_t>(length - 1), L'\0');
    const int written =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), output.data(), length);
    if (written != length) {
        throw std::runtime_error("MultiByteToWideChar truncated its result.");
    }
    return output;
}

#else

namespace {

// wchar_t is 32-bit on both non-Windows targets, so the code point is the
// character value itself and the work here is plain UTF-8 encode/decode.
void AppendUtf8(std::string& output, const std::uint32_t code_point) {
    if (code_point <= 0x7F) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

} // namespace

std::string ToUtf8(std::wstring_view text) {
    std::string output;
    output.reserve(text.size());
    for (const wchar_t character : text) {
        AppendUtf8(output, static_cast<std::uint32_t>(character));
    }
    return output;
}

std::wstring ToWide(std::string_view text) {
    std::wstring output;
    output.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        std::uint32_t code_point = 0;
        int extra_bytes = 0;
        if (first < 0x80) {
            code_point = first;
        } else if ((first & 0xE0) == 0xC0) {
            code_point = first & 0x1F;
            extra_bytes = 1;
        } else if ((first & 0xF0) == 0xE0) {
            code_point = first & 0x0F;
            extra_bytes = 2;
        } else if ((first & 0xF8) == 0xF0) {
            code_point = first & 0x07;
            extra_bytes = 3;
        } else {
            throw std::runtime_error("ToWide: malformed UTF-8 input.");
        }
        if (index + 1 + static_cast<std::size_t>(extra_bytes) > text.size()) {
            throw std::runtime_error("ToWide: truncated UTF-8 sequence.");
        }
        for (int position = 1; position <= extra_bytes; ++position) {
            const auto continuation = static_cast<unsigned char>(text[index + position]);
            if ((continuation & 0xC0) != 0x80) {
                throw std::runtime_error("ToWide: malformed UTF-8 continuation byte.");
            }
            code_point = (code_point << 6) | (continuation & 0x3F);
        }
        index += 1 + static_cast<std::size_t>(extra_bytes);
        output.push_back(static_cast<wchar_t>(code_point));
    }
    return output;
}

#endif

} // namespace osss
