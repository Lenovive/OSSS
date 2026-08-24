#pragma once

#include <string>
#include <string_view>

namespace osss {

// Conversions between the platform's wide string type and UTF-8.
//
// On Windows wchar_t is UTF-16, so the conversion goes through the wide-char
// APIs. On macOS and Linux wchar_t is 32-bit, so the conversion is a straight
// encode/decode. Core code works in std::string (UTF-8); platform-boundary
// code is the only place std::wstring appears in a core call.
[[nodiscard]] std::string ToUtf8(std::wstring_view text);

[[nodiscard]] std::wstring ToWide(std::string_view text);

} // namespace osss
