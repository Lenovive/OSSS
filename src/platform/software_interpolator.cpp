#include "platform/software_interpolator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace osss {
namespace {

constexpr int kSearchRadius = 48;
constexpr int kSearchStep = 2;
constexpr int kSampleStride = 8;
constexpr double kSceneCutMeanLumaDelta = 72.0;

std::uint8_t Channel(const std::uint32_t pixel, const unsigned shift) noexcept {
    return static_cast<std::uint8_t>((pixel >> shift) & 0xFFU);
}

} // namespace

void SoftwareInterpolator::SetMask(const std::span<const std::uint8_t> coverage) {
    mask_.assign(coverage.begin(), coverage.end());
}

void SoftwareInterpolator::ClearMask() noexcept {
    mask_.clear();
}

double SoftwareInterpolator::Luma(const std::uint32_t pixel) noexcept {
    return 0.2126 * static_cast<double>(Channel(pixel, 16)) +
        0.7152 * static_cast<double>(Channel(pixel, 8)) +
        0.0722 * static_cast<double>(Channel(pixel, 0));
}

std::uint32_t SoftwareInterpolator::Pack(
    const double red,
    const double green,
    const double blue) noexcept {
    const auto channel = [](const double value) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0, 255.0)));
    };
    return 0xFF000000U | (channel(red) << 16U) | (channel(green) << 8U) | channel(blue);
}

std::uint32_t SoftwareInterpolator::Sample(
    const PixelFrame& frame,
    const double x,
    const double y) {
    if (!frame.IsValid()) {
        return 0xFF000000U;
    }
    const double clamped_x = std::clamp(x, 0.0, static_cast<double>(frame.width - 1));
    const double clamped_y = std::clamp(y, 0.0, static_cast<double>(frame.height - 1));
    const auto x0 = static_cast<std::uint32_t>(std::floor(clamped_x));
    const auto y0 = static_cast<std::uint32_t>(std::floor(clamped_y));
    const auto x1 = std::min(x0 + 1U, frame.width - 1U);
    const auto y1 = std::min(y0 + 1U, frame.height - 1U);
    const double fx = clamped_x - static_cast<double>(x0);
    const double fy = clamped_y - static_cast<double>(y0);
    const auto at = [&frame](const std::uint32_t px, const std::uint32_t py) {
        return frame.pixels[static_cast<std::size_t>(py) * frame.width + px];
    };
    const std::uint32_t p00 = at(x0, y0);
    const std::uint32_t p10 = at(x1, y0);
    const std::uint32_t p01 = at(x0, y1);
    const std::uint32_t p11 = at(x1, y1);
    // Keep the four source pixels in the lambda's capture without relying on a
    // mutable global: this spelling also keeps the channel math easy to audit.
    const auto channel = [p00, p10, p01, p11, fx, fy](const unsigned shift) {
        const double top = static_cast<double>((p00 >> shift) & 0xFFU) * (1.0 - fx) +
            static_cast<double>((p10 >> shift) & 0xFFU) * fx;
        const double bottom = static_cast<double>((p01 >> shift) & 0xFFU) * (1.0 - fx) +
            static_cast<double>((p11 >> shift) & 0xFFU) * fx;
        return top * (1.0 - fy) + bottom * fy;
    };
    return Pack(channel(16), channel(8), channel(0));
}

bool SoftwareInterpolator::Prepare(const PixelFrame& previous, const PixelFrame& current) {
    prepared_ = false;
    scene_cut_ = false;
    motion_x_ = 0;
    motion_y_ = 0;
    error_.clear();
    if (!previous.IsValid() || !current.IsValid() || previous.width != current.width ||
        previous.height != current.height) {
        error_ = "software interpolator requires two equally sized non-empty frames";
        return false;
    }
    previous_ = &previous;
    current_ = &current;

    double total_delta = 0.0;
    std::size_t samples = 0;
    for (std::uint32_t y = 0; y < previous.height; y += kSampleStride) {
        for (std::uint32_t x = 0; x < previous.width; x += kSampleStride) {
            const std::size_t index = static_cast<std::size_t>(y) * previous.width + x;
            if (mask_.size() == previous.pixels.size() && mask_[index] >= 128U) {
                continue;
            }
            total_delta += std::abs(Luma(previous.pixels[index]) - Luma(current.pixels[index]));
            ++samples;
        }
    }
    scene_cut_ = samples != 0 && total_delta / static_cast<double>(samples) > kSceneCutMeanLumaDelta;

    if (mode_ == Mode::motion && !scene_cut_) {
        const auto evaluate = [this, &previous, &current](
                                  const int dx,
                                  const int dy,
                                  const std::uint32_t stride) {
            double error = 0.0;
            std::size_t compared = 0;
            for (std::uint32_t y = stride; y + stride < previous.height; y += stride) {
                for (std::uint32_t x = stride; x + stride < previous.width; x += stride) {
                    const std::size_t sample_index = static_cast<std::size_t>(y) * previous.width + x;
                    if (mask_.size() == previous.pixels.size() && mask_[sample_index] >= 128U) {
                        continue;
                    }
                    const int sx = static_cast<int>(x) + dx;
                    const int sy = static_cast<int>(y) + dy;
                    if (sx < 0 || sy < 0 || sx >= static_cast<int>(current.width) ||
                        sy >= static_cast<int>(current.height)) {
                        continue;
                    }
                    const auto a = previous.pixels[static_cast<std::size_t>(y) * previous.width + x];
                    const auto b = current.pixels[static_cast<std::size_t>(sy) * current.width + sx];
                    error += std::abs(Luma(a) - Luma(b));
                    ++compared;
                }
            }
            if (compared == 0) {
                return std::numeric_limits<double>::max();
            }
            error /= static_cast<double>(compared);
            // Prefer the smallest displacement when a flat region ties.
            return error + 0.005 * std::hypot(
                static_cast<double>(dx), static_cast<double>(dy));
        };

        double best_error = std::numeric_limits<double>::max();
        for (int dy = -kSearchRadius; dy <= kSearchRadius; dy += kSearchStep) {
            for (int dx = -kSearchRadius; dx <= kSearchRadius; dx += kSearchStep) {
                const double error = evaluate(dx, dy, kSampleStride * 2U);
                if (error < best_error) {
                    best_error = error;
                    motion_x_ = dx;
                    motion_y_ = dy;
                }
            }
        }

        // The coarse grid intentionally skips most pixels. A narrow moving
        // feature can therefore make a range of integer translations look
        // equally good (every probe lands inside the feature). Re-score a small
        // neighbourhood on a denser grid so its leading/trailing edges break
        // that tie without making the full search quadratic at 4K.
        const int coarse_x = motion_x_;
        const int coarse_y = motion_y_;
        const std::uint64_t pixel_count =
            static_cast<std::uint64_t>(previous.width) * previous.height;
        const std::uint32_t refine_stride = pixel_count > 4'000'000ULL
            ? 16U
            : std::max<std::uint32_t>(
                4U,
                std::max(previous.width, previous.height) > 1920U ? 8U : 4U);
        double refined_error = std::numeric_limits<double>::max();
        for (int dy = coarse_y - 8; dy <= coarse_y + 8; dy += kSearchStep) {
            for (int dx = coarse_x - 8; dx <= coarse_x + 8; dx += kSearchStep) {
                if (dx < -kSearchRadius || dx > kSearchRadius ||
                    dy < -kSearchRadius || dy > kSearchRadius) {
                    continue;
                }
                const double error = evaluate(dx, dy, refine_stride);
                if (error < refined_error) {
                    refined_error = error;
                    motion_x_ = dx;
                    motion_y_ = dy;
                }
            }
        }
    }
    prepared_ = true;
    return true;
}

PixelFrame SoftwareInterpolator::Render(const float alpha) const {
    PixelFrame output;
    if (!prepared_ || !previous_ || !current_) {
        return output;
    }
    output.Reset(current_->width, current_->height);
    output.media_time = current_->media_time;
    const double t = std::clamp(static_cast<double>(alpha), 0.0, 1.0);
    if (scene_cut_) {
        output.pixels = current_->pixels;
        return output;
    }
    if (mode_ == Mode::blend) {
        for (std::size_t index = 0; index < output.pixels.size(); ++index) {
            const auto a = previous_->pixels[index];
            const auto b = current_->pixels[index];
            const std::uint32_t blended = Pack(
                static_cast<double>(Channel(a, 16)) * (1.0 - t) + static_cast<double>(Channel(b, 16)) * t,
                static_cast<double>(Channel(a, 8)) * (1.0 - t) + static_cast<double>(Channel(b, 8)) * t,
                static_cast<double>(Channel(a, 0)) * (1.0 - t) + static_cast<double>(Channel(b, 0)) * t);
            const double newest_weight = mask_.size() == output.pixels.size()
                ? static_cast<double>(mask_[index]) / 255.0
                : 0.0;
            output.pixels[index] = newest_weight <= 0.0
                ? blended
                : newest_weight >= 1.0
                    ? b
                    : Pack(
                        static_cast<double>(Channel(blended, 16)) * (1.0 - newest_weight) +
                            static_cast<double>(Channel(b, 16)) * newest_weight,
                        static_cast<double>(Channel(blended, 8)) * (1.0 - newest_weight) +
                            static_cast<double>(Channel(b, 8)) * newest_weight,
                        static_cast<double>(Channel(blended, 0)) * (1.0 - newest_weight) +
                            static_cast<double>(Channel(b, 0)) * newest_weight);
        }
        return output;
    }

    for (std::uint32_t y = 0; y < output.height; ++y) {
        for (std::uint32_t x = 0; x < output.width; ++x) {
            // `motion` is the displacement from the previous frame to the
            // current frame. Pull each endpoint back to the requested time.
            const double px = static_cast<double>(x) - static_cast<double>(motion_x_) * t;
            const double py = static_cast<double>(y) - static_cast<double>(motion_y_) * t;
            const double cx = static_cast<double>(x) + static_cast<double>(motion_x_) * (1.0 - t);
            const double cy = static_cast<double>(y) + static_cast<double>(motion_y_) * (1.0 - t);
            const auto a = Sample(*previous_, px, py);
            const auto b = Sample(*current_, cx, cy);
            const std::size_t index = static_cast<std::size_t>(y) * output.width + x;
            const std::uint32_t warped = Pack(
                static_cast<double>(Channel(a, 16)) * (1.0 - t) + static_cast<double>(Channel(b, 16)) * t,
                static_cast<double>(Channel(a, 8)) * (1.0 - t) + static_cast<double>(Channel(b, 8)) * t,
                static_cast<double>(Channel(a, 0)) * (1.0 - t) + static_cast<double>(Channel(b, 0)) * t);
            const double newest_weight = mask_.size() == output.pixels.size()
                ? static_cast<double>(mask_[index]) / 255.0
                : 0.0;
            output.pixels[index] = newest_weight <= 0.0
                ? warped
                : newest_weight >= 1.0
                    ? current_->pixels[index]
                    : Pack(
                        static_cast<double>(Channel(warped, 16)) * (1.0 - newest_weight) +
                            static_cast<double>(Channel(current_->pixels[index], 16)) * newest_weight,
                        static_cast<double>(Channel(warped, 8)) * (1.0 - newest_weight) +
                            static_cast<double>(Channel(current_->pixels[index], 8)) * newest_weight,
                        static_cast<double>(Channel(warped, 0)) * (1.0 - newest_weight) +
                            static_cast<double>(Channel(current_->pixels[index], 0)) * newest_weight);
        }
    }
    return output;
}

} // namespace osss
