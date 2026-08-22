#pragma once

// Assertion helpers shared by every executable in tests/.
//
// Failures throw std::runtime_error rather than calling std::exit so that stack
// unwinding runs. Several tests hold D3D11 devices and other COM objects through
// winrt::com_ptr, and std::exit would skip their destructors and leak the device
// past the reporting line. Each test's main() therefore wraps its calls in
// try/catch, prints "FAILED: <what>", and returns EXIT_FAILURE.
//
// tests/input_passthrough_smoke.cpp is the one deliberate exception: it moves the
// system pointer, so its final check runs after the pointer has been restored and
// reports directly instead of throwing. Do not "fix" that ordering.

#include <cmath>
#include <stdexcept>
#include <string>

namespace osss::test {

inline void Require(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

inline void RequireNear(
    const double actual,
    const double expected,
    const double tolerance,
    const std::string& message) {
    if (std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(
            message + " (expected " + std::to_string(expected) + ", got " +
            std::to_string(actual) + ")");
    }
}

} // namespace osss::test
