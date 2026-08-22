#include "test_pattern.h"
#include "test_harness.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using osss::test::Require;

} // namespace

int main() {
    try {
        Require(osss::kTestAnimationBaseRates.size() == 12, "The test matrix must expose 12 base rates.");
        for (std::size_t index = 0; index < osss::kTestAnimationBaseRates.size(); ++index) {
            Require(
                osss::kTestAnimationBaseRates[index] == static_cast<int>((index + 1) * 10),
                "The test matrix must contain every 10 FPS step from 10 through 120.");
        }
        Require(osss::kTestGraphicsApis.size() == 4, "The DirectX source matrix must contain four APIs.");

        for (const auto api : osss::kTestGraphicsApis) {
            for (const int fps : osss::kTestAnimationBaseRates) {
                const osss::TestPatternSpec specification{
                    osss::TestPatternSpec::kDefaultWidth,
                    osss::TestPatternSpec::kDefaultHeight,
                    api,
                    fps,
                };
                const auto frame = osss::RenderTestPattern(specification, 1.125, 17);
                Require(
                    frame.size() == static_cast<std::size_t>(specification.width) * specification.height,
                    "Every API/rate combination must render a complete reference frame.");
            }
        }

        osss::TestPatternSpec specification;
        const auto reference = osss::RenderTestPattern(specification, 1.25, 75);
        const auto repeated = osss::RenderTestPattern(specification, 1.25, 75);
        const auto exact = osss::CompareTestPatternFrames(
            reference,
            repeated,
            specification.width,
            specification.height);
        Require(exact.root_mean_square_error == 0.0, "Repeated reference rendering must be pixel exact.");
        Require(std::isinf(exact.psnr_db), "A pixel-exact match must report infinite PSNR.");

        auto alternate_api_specification = specification;
        alternate_api_specification.api = osss::TestGraphicsApi::direct3d12;
        const auto alternate_api = osss::RenderTestPattern(alternate_api_specification, 1.25, 75);
        const auto api_content = osss::CompareTestPatternFrames(
            reference,
            alternate_api,
            specification.width,
            specification.height);
        Require(
            api_content.root_mean_square_error == 0.0,
            "All DirectX APIs must share the same scored reference pixels.");

        const auto next_motion = osss::RenderTestPattern(specification, 1.255, 75);
        const auto moving = osss::CompareTestPatternFrames(
            reference,
            next_motion,
            specification.width,
            specification.height);
        Require(moving.root_mean_square_error > 0.1, "Sub-frame motion must change the scored image.");

        const auto scene_cut = osss::RenderTestPattern(specification, 3.25, 195);
        const auto cut_metrics = osss::CompareTestPatternFrames(
            reference,
            scene_cut,
            specification.width,
            specification.height);
        Require(cut_metrics.root_mean_square_error > 20.0, "The halfway scene cut must be unmistakable.");

        const auto header_variant = osss::RenderTestPattern(specification, 1.25, 76);
        const auto header_metrics = osss::CompareTestPatternFrames(
            reference,
            header_variant,
            specification.width,
            specification.height);
        Require(
            header_metrics.root_mean_square_error == 0.0,
            "Machine-readable source-frame bits must be excluded from visual scoring.");

        auto damaged = reference;
        const std::size_t damaged_index =
            static_cast<std::size_t>(osss::TestPatternSpec::kScoredTop + 10) * specification.width + 10;
        damaged[damaged_index] ^= 0x00FFFFFFU;
        const auto damaged_metrics = osss::CompareTestPatternFrames(
            reference,
            damaged,
            specification.width,
            specification.height);
        Require(damaged_metrics.maximum_channel_error > 0, "A damaged scored pixel must be detected.");
        Require(
            damaged_metrics.pixels_over_threshold_percent > 0.0,
            "A damaged scored pixel must affect the threshold metric.");

        const auto round_trip_path = std::filesystem::temp_directory_path() /
            L"osss-test-pattern-round-trip.ppm";
        std::string error;
        Require(
            osss::WriteTestPatternPpm(
                round_trip_path,
                reference,
                specification.width,
                specification.height,
                error),
            "Writing the reference PPM failed: " + error);
        std::vector<std::uint32_t> loaded;
        std::uint32_t loaded_width = 0;
        std::uint32_t loaded_height = 0;
        Require(
            osss::ReadTestPatternPpm(
                round_trip_path,
                loaded,
                loaded_width,
                loaded_height,
                error),
            "Reading the reference PPM failed: " + error);
        std::filesystem::remove(round_trip_path);
        Require(
            loaded_width == specification.width && loaded_height == specification.height &&
                loaded == reference,
            "PPM reference export must round-trip exactly.");

        // HUD overlay fixture for UI-mask validation.
        Require(
            osss::TestPatternHudRects(specification).empty(),
            "The HUD overlay must be off by default so historical scores are unchanged.");

        auto hud_specification = specification;
        hud_specification.hud_overlay = true;
        const auto hud_rects = osss::TestPatternHudRects(hud_specification);
        Require(hud_rects.size() == 2, "The HUD fixture must expose two panels.");
        for (const auto& rect : hud_rects) {
            Require(
                rect.top >= osss::TestPatternSpec::kScoredTop,
                "HUD panels must sit inside the scored region.");
            Require(
                rect.right <= hud_specification.width && rect.bottom <= hud_specification.height,
                "HUD panels must stay inside the frame.");
        }
        Require(
            osss::TestPatternHudMaskArgument(hud_specification).find("px") != std::string::npos,
            "The HUD mask argument must use pixel syntax.");

        // The HUD depends only on the source frame index: same index at a
        // different time keeps it identical, adjacent indices differ.
        const auto hud_frame_100 = osss::RenderTestPattern(hud_specification, 1.25, 100);
        const auto hud_frame_100_later = osss::RenderTestPattern(hud_specification, 1.2583, 100);
        const auto hud_frame_101 = osss::RenderTestPattern(hud_specification, 1.2583, 101);
        const auto same_index = osss::CheckTestPatternHud(hud_frame_100_later, hud_specification, 100, 100);
        Require(
            same_index.matches_discrete_state && same_index.maximum_channel_error == 0,
            "A HUD panel must depend only on the source frame index, not the animation time.");
        const auto wrong_index = osss::CheckTestPatternHud(hud_frame_101, hud_specification, 100, 100);
        Require(
            !wrong_index.matches_discrete_state && wrong_index.maximum_channel_error > 64,
            "Adjacent source frames must drive visibly different HUD cells.");

        // The whole point of the fixture: a blended HUD matches no real state.
        auto blended = hud_frame_100;
        for (const auto& rect : hud_rects) {
            for (std::uint32_t y = rect.top; y < rect.bottom; ++y) {
                for (std::uint32_t x = rect.left; x < rect.right; ++x) {
                    const std::size_t index =
                        static_cast<std::size_t>(y) * hud_specification.width + x;
                    std::uint32_t mixed = 0xFF000000U;
                    for (const unsigned shift : {0U, 8U, 16U}) {
                        const unsigned a = (hud_frame_100[index] >> shift) & 0xFFU;
                        const unsigned b = (hud_frame_101[index] >> shift) & 0xFFU;
                        mixed |= ((a + b) / 2U) << shift;
                    }
                    blended[index] = mixed;
                }
            }
        }
        const auto blended_check = osss::CheckTestPatternHud(blended, hud_specification, 98, 103);
        Require(
            !blended_check.matches_discrete_state && blended_check.maximum_channel_error > 64,
            "A cross-faded HUD must match no real source-frame state.");
        const auto clean_check = osss::CheckTestPatternHud(hud_frame_100, hud_specification, 98, 103);
        Require(
            clean_check.matches_discrete_state && clean_check.matched_source_frame_index == 100,
            "An unblended HUD must be identified as exactly one real source frame.");
        Require(clean_check.compared_pixels > 0, "The HUD check must compare pixels.");

        // Excluding the HUD must isolate it from the moving-content metrics.
        // Same animation time, different source frame index: the HUD panels are
        // then the only scored difference between the two frames.
        const auto hud_same_time_101 = osss::RenderTestPattern(hud_specification, 1.25, 101);
        const auto hud_polluted = osss::CompareTestPatternFrames(
            hud_frame_100,
            hud_same_time_101,
            hud_specification.width,
            hud_specification.height);
        const auto hud_excluded = osss::CompareTestPatternFrames(
            hud_frame_100,
            hud_same_time_101,
            hud_specification.width,
            hud_specification.height,
            osss::TestPatternSpec::kScoredTop,
            8,
            1,
            hud_rects);
        Require(
            hud_polluted.maximum_channel_error > 0,
            "Differing HUD states must show up when the region is scored.");
        Require(
            hud_excluded.root_mean_square_error == 0.0,
            "Excluding the HUD regions must remove them from the moving-content score.");
        Require(
            hud_excluded.compared_pixels < hud_polluted.compared_pixels,
            "Exclusion must reduce the compared-pixel count.");

        // Source-frame phase: the burst scorer's real-versus-generated verdict.
        // A real frame is presented for exactly k / base_fps, so its phase is
        // zero; a generated frame lies strictly between two source instants.
        using osss::test::RequireNear;
        RequireNear(
            osss::SourceFramePhaseMilliseconds(100.0 / 60.0, 60),
            0.0,
            1e-9,
            "A source instant must have zero phase.");
        RequireNear(
            osss::SourceFramePhaseMilliseconds(100.25 / 60.0, 60),
            0.25 / 60.0 * 1000.0,
            1e-9,
            "A quarter-frame after a source instant must report +4.17 ms at 60 FPS.");
        RequireNear(
            osss::SourceFramePhaseMilliseconds(99.75 / 60.0, 60),
            -0.25 / 60.0 * 1000.0,
            1e-9,
            "A quarter-frame before a source instant must report -4.17 ms at 60 FPS.");
        // Exactly half way is equidistant from both instants; only the
        // magnitude is defined there.
        RequireNear(
            std::abs(osss::SourceFramePhaseMilliseconds(0.5 / 20.0, 20)),
            25.0,
            1e-9,
            "Half a frame at 20 FPS is 25 ms from the nearest source instant.");
        Require(
            osss::IsSourceFrameInstant(100.0 / 60.0, 60),
            "An exact source instant is a real frame.");
        Require(
            osss::IsSourceFrameInstant(100.0 / 60.0 + 0.00025, 60),
            "A match within the fine temporal-search step is still a real frame.");
        Require(
            !osss::IsSourceFrameInstant(100.5 / 60.0, 60),
            "A half-frame instant is a generated frame.");
        // The tightest generated spacing the harness can meet: a 120 FPS source
        // at 6x places generated frames 1.39 ms apart, so the first generated
        // slot must still clear the default tolerance.
        Require(
            !osss::IsSourceFrameInstant(1.0 / 120.0 / 6.0, 120),
            "The first 6x slot of a 120 FPS source must classify as generated.");
        Require(
            osss::SourceFramePhaseMilliseconds(1.0, 0) == 0.0,
            "A non-positive base rate must not divide by zero.");

        std::cout
            << "OSSS test-animation references passed: 4 DirectX APIs x 12 base rates, "
               "deterministic motion/cut metrics, HUD-overlay mask fixture, source-frame "
               "phase, and exact PPM round-trip.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
    }
    return EXIT_FAILURE;
}
