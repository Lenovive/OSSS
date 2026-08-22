#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace osss {

// Writes a temporally ordered run of frames as PNGs plus a self-contained HTML
// viewer for stepping through them.
//
// Every image this repo produced before was one frame in isolation, and every
// score it reports about a run is a scalar averaged over the whole run. Neither
// can answer the question a flicker complaint asks -- *which* frame, and
// *where* -- because flicker is a property of the difference between
// consecutive frames and nothing here ever held two of them at once.
//
// The viewer is the point, not the PNGs. A directory of numbered images is not
// meaningfully better than a directory of numbered PPMs; being able to step,
// loop, and A/B-blink them at a chosen rate is what turns a temporal artifact
// into something visible. See RenderErrorStepView below for the one image that
// shows flicker directly rather than by inference.

// Per-pixel signed luma error, observed minus expected, in 8-bit luma units.
// The shared input of both diagnostic views below, exposed because a caller
// scoring the same frames should not compute it twice.
[[nodiscard]] std::vector<double> LumaError(
    std::span<const std::uint32_t> observed,
    std::span<const std::uint32_t> expected);

// Signed error as a diverging ramp on black: red where the frame is brighter
// than the truth, blue where it is darker. Ghosting shows as a red/blue pair
// straddling a moving edge, which an absolute-error heat map cannot
// distinguish from ordinary softness.
[[nodiscard]] std::vector<std::uint32_t> RenderErrorView(
    std::span<const double> error,
    double gain = 6.0);

// The flicker map: how much the error changed since the previous frame of the
// sequence, as a black-red-yellow-white heat ramp.
//
// This is the only image here that shows a temporal artifact directly. A frame
// that is softly wrong in a steady way is nearly invisible in motion and prints
// black here; the same average error appearing and disappearing every frame is
// what reads as shimmer, and prints bright. `previous` empty (the first frame
// of a run) yields an all-black frame, which is correct: nothing changed
// because there was nothing to change from.
[[nodiscard]] std::vector<std::uint32_t> RenderErrorStepView(
    std::span<const double> error,
    std::span<const double> previous_error,
    double gain = 12.0);

// One named image of one frame. `pixels` is borrowed for the duration of the
// AddFrame call only.
struct SequenceView {
    std::string name;
    std::span<const std::uint32_t> pixels;
};

// One region's frame-to-frame behaviour, as reported next to the frame it
// belongs to. `error_step` is this frame's mean absolute change in the error
// signal since the previous frame -- the per-frame term of the `error-step`
// column that the quality bench otherwise reports only as a run-wide mean.
struct SequenceMetric {
    std::string name;
    double error_step = 0.0;
    double crossfade_error_step = 0.0;
    double worst_error_step = 0.0;
    std::uint32_t worst_x = 0;
    std::uint32_t worst_y = 0;
};

struct SequenceFrame {
    int index = 0;
    int pair = 0;
    double seconds = 0.0;
    float alpha = 0.0F;
    // Free-form; shown in the viewer next to the frame number. The reach ramp
    // uses it for the pan distance, which is what distinguishes its frames.
    std::string label;
    std::vector<SequenceMetric> metrics;
};

// Collects frames into a directory and writes the viewer over them.
//
// PNGs are written as frames arrive rather than buffered: a run of 24 frames at
// eight views each is 192 images, and holding them all costs more memory than
// the bench that produced them.
class FrameSequenceWriter {
public:
    // `embed_divisor` of 0 disables the single-file viewer. Otherwise a second
    // viewer is written with every image inlined as a data URI, box-downscaled
    // by that divisor -- 1 for full resolution, 2 or 4 to keep a shareable file
    // inside a size ceiling.
    explicit FrameSequenceWriter(
        std::filesystem::path directory,
        std::uint32_t embed_divisor = 0);

    // Appends one frame to the named sequence. Sequences appear in the viewer
    // in the order they are first seen, and frames within one in call order.
    bool AddFrame(
        const std::string& sequence,
        const SequenceFrame& frame,
        const std::vector<SequenceView>& views,
        std::uint32_t width,
        std::uint32_t height,
        std::string& error);

    // Writes viewer.html, and viewer-embedded.html when embedding is on.
    bool WriteViewer(const std::string& title, std::string& error);

    [[nodiscard]] std::size_t FrameCount() const noexcept {
        return frame_count_;
    }

    [[nodiscard]] std::size_t ImageCount() const noexcept {
        return image_count_;
    }

    [[nodiscard]] std::uintmax_t BytesWritten() const noexcept {
        return bytes_written_;
    }

    [[nodiscard]] const std::filesystem::path& Directory() const noexcept {
        return directory_;
    }

private:
    struct Record {
        SequenceFrame frame;
        std::vector<std::string> view_names;
        std::vector<std::string> files;
        std::vector<std::string> embedded;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    struct Sequence {
        std::string name;
        std::vector<Record> records;
    };

    [[nodiscard]] std::string BuildViewerHtml(const std::string& title, bool embedded) const;

    std::filesystem::path directory_;
    std::uint32_t embed_divisor_ = 0;
    std::vector<Sequence> sequences_;
    std::size_t frame_count_ = 0;
    std::size_t image_count_ = 0;
    std::uintmax_t bytes_written_ = 0;
};

} // namespace osss
