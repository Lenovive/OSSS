#pragma once

#include "pixel_frame.h"

#include <cstdint>
#include <string>
#include <span>
#include <vector>

namespace osss {

// Dependency-free fallback used by the non-Windows desktop backends. It keeps
// the same semantic contract as the D3D11 interpolator: a source pair is
// prepared once, then any number of positions in that pair can be rendered.
// The estimator deliberately uses a bounded global translation model. It is
// not presented as equivalent to the dense HLSL flow path; it is a stable
// software backend that remains useful on machines without a common GPU API.
class SoftwareInterpolator {
public:
    enum class Mode {
        blend,
        motion,
    };

    void SetMode(Mode mode) noexcept {
        mode_ = mode;
    }

    // Coverage is a tightly packed source-sized mask: zero means the pixel
    // may be reconstructed, 255 means it must come from the newest real frame,
    // and intermediate values blend the two. The mask is copied so callers can
    // rebuild it when a captured window changes size.
    void SetMask(std::span<const std::uint8_t> coverage);
    void ClearMask() noexcept;

    [[nodiscard]] Mode GetMode() const noexcept {
        return mode_;
    }

    // Returns false only for incompatible/empty source frames. A scene cut is
    // a successful prepare whose Render result is the newest real frame.
    bool Prepare(const PixelFrame& previous, const PixelFrame& current);

    [[nodiscard]] bool Ready() const noexcept {
        return prepared_;
    }

    [[nodiscard]] PixelFrame Render(float alpha) const;

    [[nodiscard]] int MotionX() const noexcept {
        return motion_x_;
    }

    [[nodiscard]] int MotionY() const noexcept {
        return motion_y_;
    }

    [[nodiscard]] bool SceneCut() const noexcept {
        return scene_cut_;
    }

    [[nodiscard]] const std::string& LastError() const noexcept {
        return error_;
    }

private:
    [[nodiscard]] static std::uint32_t Sample(const PixelFrame& frame, double x, double y);
    [[nodiscard]] static double Luma(std::uint32_t pixel) noexcept;
    [[nodiscard]] static std::uint32_t Pack(double red, double green, double blue) noexcept;

    Mode mode_ = Mode::motion;
    const PixelFrame* previous_ = nullptr;
    const PixelFrame* current_ = nullptr;
    bool prepared_ = false;
    bool scene_cut_ = false;
    int motion_x_ = 0;
    int motion_y_ = 0;
    std::string error_;
    std::vector<std::uint8_t> mask_;
};

} // namespace osss
