#include "motion_interpolator.h"

#include "shader_cache.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace osss {
namespace {

constexpr DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr DXGI_FORMAT kLumaFormat = DXGI_FORMAT_R16_FLOAT;
constexpr UINT kThreadGroupSize = 8;

constexpr char kMotionComputeShaderSourceHead[] = R"(
cbuffer MotionConstants : register(b0) {
    uint2 source_size;
    uint2 coarse_size;
    uint2 fine_size;
    float coarse_scale;
    float fine_scale;
    float2 inverse_source_size;
    uint ui_mask_enabled;
    uint auto_mask_enabled;
    // Pyramid level each stage matches on: log2 of its search step for the
    // coarse level, full resolution for the fine level.
    float coarse_mip;
    float fine_mip;
    // Half-width of the coarse search, in coarse cells; ResolveCoarseSearchRadius
    // in src/flow_scale.h decides it, from the measured source period and the
    // flow divisor together -- the target it works back from is a reach in
    // source pixels, so a finer grid raises this rather than shortening reach.
    // Runtime rather than a #define because of that: both loops below are already
    // dynamic ([loop]), so a dynamic bound costs nothing in codegen, whereas
    // specialising it would recompile the motion shaders every time the source
    // rate drifted across a boundary -- a visible hitch mid-gameplay in exchange
    // for nothing.
    int coarse_radius;
    // Temporal prior. `flow_prior` (t7) holds the previous pair's *backward*
    // fine flow, which lives on the grid of the frame both pairs share and
    // points the way that pair moved. Negated it predicts this pair's forward
    // motion; as is, it approximates the backward. So -1 for the forward
    // dispatch, +1 for the backward, 0 when there is no usable prior (first
    // pair, discontinuity, disabled).
    float flow_prior_sign;
};

// Below this the previous pair's vector is not trusted as a search seed. Flat
// regions report ~0.25 with zero motion, so the gate also skips them.
static const float kFlowPriorConfidence = 0.30f;

// Automatic static-overlay detection. A cell is UI-like when its own pixels
// hold still while the neighbourhood around it moves. Persistence rises slowly
// so ordinary quiet gameplay cannot arm it, and falls faster than it rises so a
// region that starts moving is released within a few source pairs.
static const float kAutoStaticDifference = 0.010f;  // per-channel mean, 0-1
static const float kAutoNeighbourMotion = 1.25f;    // source pixels per pair
static const float kAutoMotionDecay = 0.80f;        // ~11 pairs of motion memory

// How close two neighbouring flow vectors must be, in source pixels, to count
// as agreeing when hunting for isolated outliers.
static const float kFlowAgreePixels = 2.5f;

// How many of the eight neighbours may agree with a cell before it stops
// counting as isolated. Raising it filters more aggressively and starts eating
// genuine motion boundaries.
static const uint kFlowIsolatedNeighbours = 1;
static const float kAutoRise = 0.10f;               // ~10 pairs to fully arm
static const float kAutoFall = 0.34f;               // ~3 pairs to release
static const float kAutoMaskLow = 0.55f;
static const float kAutoMaskHigh = 0.85f;

Texture2D<float4> frame_a : register(t0);
Texture2D<float4> frame_b : register(t1);
Texture2D<float4> coarse_flow : register(t2);
// Static UI/HUD coverage shared by both frames: 1 = excluded from motion.
Texture2D<float> ui_mask : register(t3);
// Detector state carried across source pairs, at fine-flow resolution.
Texture2D<float4> ui_persistence : register(t4);
// Mip-chained luma of the same two frames. Every patch comparison reads these
// rather than the colour frames, so each pyramid level searches a band-limited
// image instead of point-sampling full-resolution pixels on a coarse grid.
Texture2D<float> luma_a : register(t5);
Texture2D<float> luma_b : register(t6);
// The previous pair's filtered backward fine flow; see flow_prior_sign.
Texture2D<float4> flow_prior : register(t7);
SamplerState linear_clamp : register(s0);
RWTexture2D<float4> motion_output : register(u0);
RWTexture2D<float> luma_a_output : register(u1);
RWTexture2D<float> luma_b_output : register(u2);

float AutoMaskFromScore(float score) {
    return smoothstep(kAutoMaskLow, kAutoMaskHigh, score);
}

float Luma(float3 color) {
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

float SampleA(float2 pixel, float mip) {
    return luma_a.SampleLevel(linear_clamp, (pixel + 0.5f) * inverse_source_size, mip);
}

float SampleB(float2 pixel, float mip) {
    return luma_b.SampleLevel(linear_clamp, (pixel + 0.5f) * inverse_source_size, mip);
}

// Fills mip 0 of both luma pyramids; the remaining levels come from
// GenerateMips. Separating luma out once per pair is also cheaper than
// recomputing it inside every one of the thousands of patch comparisons below.
[numthreads(8, 8, 1)]
void BuildLuma(uint3 dispatch_id : SV_DispatchThreadID) {
    if (dispatch_id.x >= source_size.x || dispatch_id.y >= source_size.y) {
        return;
    }
    const float2 uv = (float2(dispatch_id.xy) + 0.5f) * inverse_source_size;
    luma_a_output[dispatch_id.xy] = Luma(frame_a.SampleLevel(linear_clamp, uv, 0).rgb);
    luma_b_output[dispatch_id.xy] = Luma(frame_b.SampleLevel(linear_clamp, uv, 0).rgb);
}

// Combined exclusion at a source pixel: user rectangles plus, when enabled,
// the automatically detected static overlay from the previous update.
float MaskAt(float2 pixel) {
    const float2 uv = (pixel + 0.5f) * inverse_source_size;
    float mask = ui_mask_enabled != 0 ? ui_mask.SampleLevel(linear_clamp, uv, 0) : 0.0f;
    if (auto_mask_enabled != 0) {
        // .y is the dilated score; .x is the raw accumulation the update reads.
        mask = max(mask, AutoMaskFromScore(ui_persistence.SampleLevel(linear_clamp, uv, 0).y));
    }
    return mask;
}

bool AnyMaskEnabled() {
    return ui_mask_enabled != 0 || auto_mask_enabled != 0;
}

// Weight of one comparison tap: both the reference sample and the candidate
// sample must lie outside masked UI so a static HUD cannot vote for zero
// motion on behalf of the moving content around it.
float TapWeight(float2 reference, float2 candidate) {
    return (1.0f - MaskAt(reference)) * (1.0f - MaskAt(candidate));
}

float PatchError(float2 center, float2 motion, float radius, float mip) {
    const float2 moved = center + motion;
    if (moved.x < radius || moved.y < radius ||
        moved.x >= (float)source_size.x - radius ||
        moved.y >= (float)source_size.y - radius) {
        return 4.0f;
    }

    const float2 taps[5] = {
        float2(0.0f, 0.0f),
        float2(-radius, 0.0f),
        float2(radius, 0.0f),
        float2(0.0f, -radius),
        float2(0.0f, radius)
    };

    if (!AnyMaskEnabled()) {
        float error = 0.0f;
        [unroll]
        for (uint tap = 0; tap < 5; ++tap) {
            error += abs(SampleA(center + taps[tap], mip) - SampleB(moved + taps[tap], mip));
        }
        return error * 0.2f;
    }

    float weighted_error = 0.0f;
    float weight_sum = 0.0f;
    [unroll]
    for (uint tap = 0; tap < 5; ++tap) {
        const float weight = TapWeight(center + taps[tap], moved + taps[tap]);
        weighted_error += weight *
            abs(SampleA(center + taps[tap], mip) - SampleB(moved + taps[tap], mip));
        weight_sum += weight;
    }
    // Fewer than roughly one unmasked tap leaves no evidence; treat the
    // candidate like an out-of-bounds match so it cannot win by default.
    if (weight_sum < 0.75f) {
        return 4.0f;
    }
    return weighted_error / weight_sum;
}

// A flow sample whose centre lies in masked UI carries no motion and no
// confidence, so the fusion pass falls back to real pixels around it.
bool CenterMasked(float2 center) {
    return AnyMaskEnabled() && MaskAt(center) > 0.5f;
}

// How much structure the reference patch actually contains, as the mean
// absolute deviation of the same taps PatchError compares. Match error is only
// meaningful relative to this: a fixed absolute threshold rates every textured
// patch a bad match, drives confidence to zero, and silently turns the whole
// interpolator back into the crossfade it is supposed to replace.
float PatchContrast(float2 center, float radius, float mip) {
    const float2 taps[5] = {
        float2(0.0f, 0.0f),
        float2(-radius, 0.0f),
        float2(radius, 0.0f),
        float2(0.0f, -radius),
        float2(0.0f, radius)
    };
    float samples[5];
    float mean = 0.0f;
    [unroll]
    for (uint tap = 0; tap < 5; ++tap) {
        samples[tap] = SampleA(center + taps[tap], mip);
        mean += samples[tap];
    }
    mean *= 0.2f;

    float deviation = 0.0f;
    [unroll]
    for (uint index = 0; index < 5; ++index) {
        deviation += abs(samples[index] - mean);
    }
    return deviation * 0.2f;
}

float FlowConfidence(float best_error, float second_error, float contrast) {
    // Residual error is judged against the patch's own contrast, floored so a
    // flat patch cannot claim a perfect match on the strength of having nothing
    // in it. The floor is the noise level a capture surface can carry.
    const float scale = max(contrast, 0.025f);
    const float appearance = saturate(1.0f - best_error / scale);
    const float ambiguity = max(second_error, best_error);
    const float uniqueness = saturate((ambiguity - best_error) / max(ambiguity, 0.002f) * 2.0f);
    return appearance * lerp(0.25f, 1.0f, uniqueness);
}

// Selection cost: photometric error plus a small penalty on how far a candidate
// strays from `anchor`, measured in units of the search step.
//
// Ties are the common case here, not the exception -- every uniform region
// matches every candidate equally well -- and a plain `error < best` comparison
// awards those ties to whichever candidate the loop happens to visit first,
// which is the far corner of the search window. Forward and backward estimation
// both do that and land on opposite corners, so FlowConsistency rejects the
// pair, the fused weight falls below confidence_floor, and the interpolator
// silently degenerates into the crossfade it exists to replace. Preferring the
// smallest displacement that still explains the pixels is also the standard
// remedy for the aperture problem along an edge.
//
// The penalty is deliberately much smaller than the error gap between a right
// and a wrong match, so it decides ties and nothing else.
static const float kMotionRegularization = 0.006f;



float MotionCost(float error, float2 motion, float2 anchor, float scale) {
    return error + kMotionRegularization * length(motion - anchor) / max(scale, 1e-4f);
}

// Moves `motion` to the best of its eight neighbours at `search_step`, or leaves
// it in place. `second_error` is not updated here: it measures ambiguity across
// the wide search, and neighbours this close are always near-ties, so folding
// them in would report every correct match as ambiguous.
float2 RefineNeighbourhood(
    float2 center,
    float2 motion,
    float search_step,
    float radius,
    float mip,
    float2 anchor,
    float anchor_scale,
    inout float best_cost,
    inout float best_error) {
    float2 best_motion = motion;
    [unroll]
    for (int offset_y = -1; offset_y <= 1; ++offset_y) {
        [unroll]
        for (int offset_x = -1; offset_x <= 1; ++offset_x) {
            if (offset_x == 0 && offset_y == 0) {
                continue;
            }
            const float2 candidate = motion + float2(offset_x, offset_y) * search_step;
            const float error = PatchError(center, candidate, radius, mip);
            const float cost = MotionCost(error, candidate, anchor, anchor_scale);
            if (cost < best_cost) {
                best_cost = cost;
                best_error = error;
                best_motion = candidate;
            }
        }
    }
    return best_motion;
}

// Fits a parabola through the error samples either side of the best position on
// each axis and returns its vertex. Displacement in real content is continuous;
// without this the field is stuck on whatever grid the search used, and at
// 60 FPS the pattern's own motion is 1.5-3.8 pixels per frame, so a two-pixel
// grid is a larger error than the motion it is trying to describe.
float2 SubPixelVertex(
    float2 center,
    float2 motion,
    float search_step,
    float radius,
    float mip,
    float center_error) {
    const float left = PatchError(center, motion - float2(search_step, 0.0f), radius, mip);
    const float right = PatchError(center, motion + float2(search_step, 0.0f), radius, mip);
    const float up = PatchError(center, motion - float2(0.0f, search_step), radius, mip);
    const float down = PatchError(center, motion + float2(0.0f, search_step), radius, mip);

    float2 delta = 0.0f;
    const float horizontal_curvature = left - 2.0f * center_error + right;
    if (horizontal_curvature > 1e-5f) {
        delta.x = clamp(0.5f * (left - right) / horizontal_curvature, -0.85f, 0.85f);
    }
    const float vertical_curvature = up - 2.0f * center_error + down;
    if (vertical_curvature > 1e-5f) {
        delta.y = clamp(0.5f * (up - down) / vertical_curvature, -0.85f, 0.85f);
    }
    return motion + delta * search_step;
}

)";

// Continues the same HLSL translation unit. The pieces are split only because
// MSVC caps a single string literal at 16 KB, and they are concatenated before
// compilation, so a function declared in one may be used in the next. Keep every
// split between top-level definitions, and add another piece rather than letting
// one approach the cap. `ShaderSource[] = R"` finds all of them.
constexpr char kMotionComputeShaderSourceEstimators[] = R"(
[numthreads(8, 8, 1)]
void EstimateCoarse(uint3 dispatch_id : SV_DispatchThreadID) {
    if (dispatch_id.x >= coarse_size.x || dispatch_id.y >= coarse_size.y) {
        return;
    }

    const float2 center = (float2(dispatch_id.xy) + 0.5f) * coarse_scale;
    if (CenterMasked(center)) {
        motion_output[dispatch_id.xy] = float4(0.0f, 0.0f, 4.0f, 0.0f);
        return;
    }
    // The coarse level reads the pyramid at `coarse_mip`, where one texel spans
    // a whole search step, so its patch is sized in those texels rather than in
    // source pixels. Point-sampling full-resolution pixels on an eight-pixel
    // grid -- the previous behaviour -- aliases any detail finer than the step:
    // a six-pixel grating read that way returns a confident match one whole
    // period away from the truth, and the fine level below can only search four
    // pixels either side, so it can never escape the wrong basin.
    const float patch_radius = coarse_scale;
    // Candidates scoring the out-of-bounds/no-evidence error never win, so a
    // pixel without any usable match keeps zero motion at zero confidence.
    // Selection runs on the regularized cost; the raw errors of the winner and
    // the runner-up ride alongside it because confidence has to be judged on how
    // well the pixels actually matched, not on the displacement prior.
    float best_cost = 4.0f;
    float second_cost = 4.0f;
    float best_error = 4.0f;
    float second_error = 4.0f;
    float2 best_motion = 0.0f;

    [loop]
    for (int offset_y = -coarse_radius; offset_y <= coarse_radius; ++offset_y) {
        [loop]
        for (int offset_x = -coarse_radius; offset_x <= coarse_radius; ++offset_x) {
            const float2 motion = float2(offset_x, offset_y) * coarse_scale;
            const float error = PatchError(center, motion, patch_radius, coarse_mip);
            const float cost = MotionCost(error, motion, 0.0f, coarse_scale);
            if (cost < best_cost) {
                second_cost = best_cost;
                second_error = best_error;
                best_cost = cost;
                best_error = error;
                best_motion = motion;
            } else if (cost < second_cost) {
                second_cost = cost;
                second_error = error;
            }
        }
    }

    // Temporal prior: where the previous pair said this content was going.
    //
    // The window above encodes a fixed displacement ceiling per pair, and
    // motion past it is unrecoverable -- not estimated coarsely, simply not
    // found. But motion that fast rarely appears from nowhere: a camera pan
    // accelerates through the window before it leaves it. Seeding nine
    // candidates on the previous pair's vector lets the estimate follow a
    // trajectory out past the window, one cell of acceleration at a time,
    // for a ninth of the window's cost at radius 4.
    //
    // The prior is a candidate, not an anchor. Selection stays regularised
    // toward zero, so in the uniform regions where every displacement ties the
    // smallest still wins and a wrong vector cannot perpetuate itself; a far
    // seed only wins where the pixels genuinely say so. Skipped where the
    // previous vector was not confident, and where it is small enough that
    // the window covers it anyway.
    //
    // A prior candidate that wins demotes the old best to runner-up like any
    // other, but one that merely ties or comes second is dropped rather than
    // recorded: a seed that duplicates a grid tap must not register as a tie
    // with it, which would report every static cell as ambiguous.
    //
    // The seed is the most confident of the fine cells under this coarse cell
    // and one ring around them, read with Load -- never a filtered sample. A
    // flow field is discontinuous by nature, and this one especially: uniform
    // interiors sit at zero by the tie-break above while their edges carry
    // the motion, so a bilinear fetch straddling the two averages 16 and 0
    // into a seed of 8 that points at nothing. Reading the ring as well lets
    // an edge cell lend its vector to the flat neighbour beside it, where the
    // taps then either confirm it or lose the tie to zero as they should.
    if (flow_prior_sign != 0.0f) {
        float4 prior = float4(0.0f, 0.0f, 0.0f, 0.0f);
        const int2 fine_origin = int2(dispatch_id.xy) * 2;
        [unroll]
        for (int ring_y = -1; ring_y <= 2; ++ring_y) {
            [unroll]
            for (int ring_x = -1; ring_x <= 2; ++ring_x) {
                const int2 fine_cell = clamp(
                    fine_origin + int2(ring_x, ring_y),
                    int2(0, 0),
                    int2(fine_size) - 1);
                const float4 candidate = flow_prior.Load(int3(fine_cell, 0));
                if (candidate.w > prior.w) {
                    prior = candidate;
                }
            }
        }
        const float2 seed = prior.xy * flow_prior_sign;
        const float near_zero = 0.25f * coarse_scale;
        if (prior.w >= kFlowPriorConfidence && dot(seed, seed) > near_zero * near_zero) {
            [unroll]
            for (int prior_y = -1; prior_y <= 1; ++prior_y) {
                [unroll]
                for (int prior_x = -1; prior_x <= 1; ++prior_x) {
                    const float2 motion = seed + float2(prior_x, prior_y) * coarse_scale;
                    const float error = PatchError(center, motion, patch_radius, coarse_mip);
                    const float cost = MotionCost(error, motion, 0.0f, coarse_scale);
                    if (cost < best_cost) {
                        second_cost = best_cost;
                        second_error = best_error;
                        best_cost = cost;
                        best_error = error;
                        best_motion = motion;
                    }
                }
            }
        }
    }

    // One halving pass so the prediction handed to the fine level is within
    // half a fine step of the true displacement. The fine level only searches
    // +/-4 pixels around it, so a coarse grid error larger than that is one the
    // fine level can never recover from.
    best_motion = RefineNeighbourhood(
        center,
        best_motion,
        coarse_scale * 0.5f,
        patch_radius,
        coarse_mip,
        0.0f,
        coarse_scale,
        best_cost,
        best_error);

    motion_output[dispatch_id.xy] = float4(
        best_motion,
        best_error,
        FlowConfidence(best_error, second_error, PatchContrast(center, patch_radius, coarse_mip)));
}

struct FlowCandidate {
    float2 motion;
    float error;
    float second_error;
};

// Successive halving down to a quarter pixel, then the parabola vertex between
// the last two samples. The wide grid only decides which cell the match lives
// in; this decides where inside it, which is what removes the stepped, doubled
// edges a grid-quantised field produces. At 60 FPS the pattern's own motion is
// 1.5-3.8 pixels per frame, so the two-pixel grid the wide pass lands on is a
// larger error than the motion it is describing.
float2 RefineTo(
    float2 center,
    float2 motion,
    float refinement_step,
    float patch_radius,
    float mip,
    float2 anchor,
    inout float cost,
    inout float error) {
    float search_step = refinement_step * 0.5f;
    [unroll]
    for (uint refinement_pass = 0; refinement_pass < 2; ++refinement_pass) {
        motion = RefineNeighbourhood(
            center,
            motion,
            search_step,
            patch_radius,
            mip,
            anchor,
            refinement_step,
            cost,
            error);
        search_step *= 0.5f;
    }
    motion = SubPixelVertex(center, motion, search_step, patch_radius, mip, error);
    error = PatchError(center, motion, patch_radius, mip);
    return motion;
}

// One complete fine search seeded at `origin`: a wide grid to pick the cell,
// then RefineTo for the position inside it.
FlowCandidate SearchFrom(
    float2 center,
    float2 origin,
    float refinement_step,
    float patch_radius,
    float mip) {
    // The prior inside one search is anchored on its own origin: this level
    // refines an existing estimate, so "stay close to where you started" is the
    // smoothness assumption that belongs here.
    float best_cost = 4.0f;
    float second_cost = 4.0f;
    float best_error = 4.0f;
    float second_error = 4.0f;
    float2 best_motion = origin;

    [loop]
    for (int offset_y = -2; offset_y <= 2; ++offset_y) {
        [loop]
        for (int offset_x = -2; offset_x <= 2; ++offset_x) {
            const float2 motion = origin + float2(offset_x, offset_y) * refinement_step;
            const float error = PatchError(center, motion, patch_radius, mip);
            const float cost = MotionCost(error, motion, origin, refinement_step);
            if (cost < best_cost) {
                second_cost = best_cost;
                second_error = best_error;
                best_cost = cost;
                best_error = error;
                best_motion = motion;
            } else if (cost < second_cost) {
                second_cost = cost;
                second_error = error;
            }
        }
    }

    best_motion = RefineTo(
        center,
        best_motion,
        refinement_step,
        patch_radius,
        mip,
        origin,
        best_cost,
        best_error);

    FlowCandidate candidate;
    candidate.motion = best_motion;
    candidate.error = best_error;
    candidate.second_error = second_error;
    return candidate;
}

[numthreads(8, 8, 1)]
void RefineFlow(uint3 dispatch_id : SV_DispatchThreadID) {
    if (dispatch_id.x >= fine_size.x || dispatch_id.y >= fine_size.y) {
        return;
    }

    const float2 center = (float2(dispatch_id.xy) + 0.5f) * fine_scale;
    if (CenterMasked(center)) {
        motion_output[dispatch_id.xy] = float4(0.0f, 0.0f, 4.0f, 0.0f);
        return;
    }
    const float2 normalized_position = (center + 0.5f) * inverse_source_size;
    const float2 predicted_motion = coarse_flow.SampleLevel(linear_clamp, normalized_position, 0).xy;
    const float refinement_step = max(1.0f, fine_scale * 0.5f);
    const float patch_radius = max(1.0f, fine_scale * 0.5f);

    FlowCandidate best =
        SearchFrom(center, predicted_motion, refinement_step, patch_radius, fine_mip);

    // Periodic detail matches just as well one whole period away, and no amount
    // of low-pass filtering removes that ambiguity -- a box mip chain still
    // leaves a sixth of a six-pixel grating standing at the level the coarse
    // search reads. When the coarse level lands in the wrong period this window
    // is only four pixels wide and can never see the smaller, correct
    // displacement, so the whole region reconstructs a clean grating in the
    // wrong phase.
    //
    // Re-running the search from zero costs one more local search and settles it
    // on the same principle the tie-break already uses -- prefer the smallest
    // displacement that explains the pixels -- applied across periods rather
    // than within one. Genuine large motion keeps its estimate, because a
    // near-zero displacement cannot explain it and the photometric gap dwarfs
    // the penalty. Skipped when the prediction is already inside the window the
    // search would cover anyway.
#if OSSS_REFINE_FROM_ZERO
    if (length(predicted_motion) > refinement_step) {
        const FlowCandidate from_zero =
            SearchFrom(center, 0.0f, refinement_step, patch_radius, fine_mip);
        if (MotionCost(from_zero.error, from_zero.motion, 0.0f, refinement_step) <
            MotionCost(best.error, best.motion, 0.0f, refinement_step)) {
            best = from_zero;
        }
    }
#endif

    // What none of this fixes: a texture displaced by more than half its own
    // period is explained equally well by the smaller displacement the other
    // way, and both explanations survive every test above. Preferring the
    // smaller one is the standard assumption and is right whenever the source
    // rate samples the motion finely enough -- the regime frame generation
    // targets -- and wrong below it, where the region reconstructs sharply in
    // the wrong phase.
    //
    // Three ways of detecting that and falling back were built and measured:
    // vetoing when the two searches disagree, when they disagree at equal
    // magnitude, and the same test applied to a separately refined runner-up
    // inside one search. All three cost more across the ordinary lanes than the
    // aliased case ever returned, the last at seven times the GPU cost, because
    // at the point of decision the recoverable and unrecoverable cases are
    // genuinely indistinguishable. Do not re-add one without a measurement that
    // says otherwise.
    motion_output[dispatch_id.xy] = float4(
        best.motion,
        best.error,
        FlowConfidence(
            best.error,
            best.second_error,
            PatchContrast(center, patch_radius, fine_mip)));
}

)";

// Continues the same HLSL translation unit; see the note above.
constexpr char kMotionComputeShaderSourceDetectors[] = R"(
// Removes isolated outliers from a finished fine flow field. The unfiltered
// field is bound to the `coarse_flow` slot; the result goes to `motion_output`.
//
// A single cell that matched something spurious yields one wrong vector, and the
// fusion then pulls that pixel from somewhere unrelated: a lone dark speck
// inside a bright moving object, or the reverse. That reads far worse than a
// smoothly wrong region, because the eye finds isolated points immediately.
//
// The test is deliberately not "differs from the neighbourhood average". A cell
// on a genuine motion boundary differs from its neighbourhood too, and blurring
// those is what produces halos around everything that moves. What separates the
// two is how many neighbours agree: a boundary cell still has a whole side
// agreeing with it, while an outlier stands alone. Only cells at most one
// neighbour agrees with are replaced, and they are replaced with the vector
// median rather than the mean, which is itself edge-preserving.
[numthreads(8, 8, 1)]
void FilterFlow(uint3 dispatch_id : SV_DispatchThreadID) {
    if (dispatch_id.x >= fine_size.x || dispatch_id.y >= fine_size.y) {
        return;
    }

    float4 neighbourhood[9];
    uint count = 0;
    [unroll]
    for (int offset_y = -1; offset_y <= 1; ++offset_y) {
        [unroll]
        for (int offset_x = -1; offset_x <= 1; ++offset_x) {
            const int2 neighbour = clamp(
                int2(dispatch_id.xy) + int2(offset_x, offset_y),
                int2(0, 0),
                int2(fine_size) - 1);
            neighbourhood[count++] = coarse_flow.Load(int3(neighbour, 0));
        }
    }
    // Row-major over offsets -1..1, so the centre is index 4.
    const float4 center = neighbourhood[4];

    // Fast motion spreads more between adjacent cells than slow motion, so what
    // counts as agreement scales with it.
    const float agreement_radius = max(kFlowAgreePixels, length(center.xy) * 0.25f);
    uint agreeing = 0;
    [unroll]
    for (uint index = 0; index < 9; ++index) {
        if (index == 4) {
            continue;
        }
        if (length(neighbourhood[index].xy - center.xy) <= agreement_radius) {
            ++agreeing;
        }
    }

    if (agreeing > kFlowIsolatedNeighbours) {
        motion_output[dispatch_id.xy] = center;
        return;
    }

    // Vector median: the neighbour minimising total distance to all the others.
    float best_total = 1e9f;
    float2 median = center.xy;
    float median_confidence = center.w;
    [unroll]
    for (uint candidate = 0; candidate < 9; ++candidate) {
        float total = 0.0f;
        [unroll]
        for (uint other = 0; other < 9; ++other) {
            total += length(neighbourhood[candidate].xy - neighbourhood[other].xy);
        }
        if (total < best_total) {
            best_total = total;
            median = neighbourhood[candidate].xy;
            median_confidence = neighbourhood[candidate].w;
        }
    }

    // The replacement was not measured at this cell, so it does not inherit the
    // outlier's confidence; it carries the median's, which the fusion can still
    // reject.
    motion_output[dispatch_id.xy] = float4(median, center.z, median_confidence);
}

// One update per source pair. `coarse_flow` is bound to the fine forward flow
// and `ui_persistence` to the previous state; the result is the state the next
// pair's flow and this pair's fusion will read.
[numthreads(8, 8, 1)]
void UpdateUiPersistence(uint3 dispatch_id : SV_DispatchThreadID) {
    if (dispatch_id.x >= fine_size.x || dispatch_id.y >= fine_size.y) {
        return;
    }

    const float2 center = (float2(dispatch_id.xy) + 0.5f) * fine_scale;
    const float radius = max(1.0f, fine_scale * 0.5f);

    // How still this cell held between the two source frames.
    const float2 taps[5] = {
        float2(0.0f, 0.0f),
        float2(-radius, 0.0f),
        float2(radius, 0.0f),
        float2(0.0f, -radius),
        float2(0.0f, radius)
    };
    float difference = 0.0f;
    [unroll]
    for (uint tap = 0; tap < 5; ++tap) {
        difference += abs(SampleA(center + taps[tap], 0.0f) - SampleB(center + taps[tap], 0.0f));
    }
    difference *= 0.2f;

    // How much the scene around this cell moved. The radii are fractions of the
    // frame rather than a few cells: a minimap or a health bar is far wider
    // than any fixed cell neighbourhood, and its interior must still be able to
    // see the gameplay moving outside it.
    const float2 radii[2] = {
        float2(source_size) * 0.06f,
        float2(source_size) * 0.18f
    };
    float neighbour_motion = 0.0f;
    [unroll]
    for (uint radius_index = 0; radius_index < 2; ++radius_index) {
        [unroll]
        for (int ring_y = -1; ring_y <= 1; ++ring_y) {
            [unroll]
            for (int ring_x = -1; ring_x <= 1; ++ring_x) {
                if (ring_x == 0 && ring_y == 0) {
                    continue;
                }
                const float2 offset = float2(ring_x, ring_y) * radii[radius_index];
                const float2 uv = (center + offset + 0.5f) * inverse_source_size;
                const float4 flow = coarse_flow.SampleLevel(linear_clamp, uv, 0);
                // Only confident flow counts as evidence that the scene moved.
                const float magnitude = length(flow.xy) * step(0.15f, flow.w);
                neighbour_motion = max(neighbour_motion, magnitude);
            }
        }
    }

    // Confident flow is sparse: only the edges of a moving object carry it, so a
    // fixed ring of sample points sees the scene move on some pairs and not
    // others. Demanding evidence on every single pair, against a fall three
    // times faster than the rise, means a real overlay never arms. Remember the
    // strongest motion seen recently and let it decay instead, so intermittent
    // evidence still accumulates.
    //
    // This only became visible once the flow field was correct. Before that the
    // whole field held the same large bogus vector, so this gate was true
    // everywhere at all times and never actually gated anything.
    const float remembered_motion = ui_persistence[dispatch_id.xy].w;
    const float motion_evidence = max(neighbour_motion, remembered_motion * kAutoMotionDecay);

    const bool overlay_like =
        difference < kAutoStaticDifference && motion_evidence > kAutoNeighbourMotion;
    const float previous = ui_persistence[dispatch_id.xy].x;
    const float score = saturate(previous + (overlay_like ? kAutoRise : -kAutoFall));

    // An overlay's outline rarely lands on cell boundaries, so the cells
    // straddling its edge only ever accumulate a partial score and the mask
    // would fade out a few pixels inside the overlay. Dilate by one cell, but
    // only into cells that were themselves still this pair: that covers the
    // outline without bleeding the mask onto the moving content beside it.
    float neighbour_best = 0.0f;
    [unroll]
    for (int dilate_y = -1; dilate_y <= 1; ++dilate_y) {
        [unroll]
        for (int dilate_x = -1; dilate_x <= 1; ++dilate_x) {
            const int2 neighbour = int2(dispatch_id.xy) + int2(dilate_x, dilate_y);
            if (neighbour.x < 0 || neighbour.y < 0 ||
                neighbour.x >= (int)fine_size.x || neighbour.y >= (int)fine_size.y) {
                continue;
            }
            // Reads the previous state, never this pass's own output, so the
            // dilation cannot feed itself and creep outwards frame after frame.
            neighbour_best = max(neighbour_best, ui_persistence[uint2(neighbour)].x);
        }
    }
    const float dilated = difference < kAutoStaticDifference
        ? max(score, neighbour_best)
        : score;

    motion_output[dispatch_id.xy] = float4(score, dilated, difference, motion_evidence);
}

groupshared float scene_difference[256];
groupshared float scene_weight[256];

[numthreads(16, 16, 1)]
void MeasureScene(uint3 group_thread_id : SV_GroupThreadID) {
    const uint flat_index = group_thread_id.y * 16 + group_thread_id.x;
    float difference = 0.0f;
    float weight = 0.0f;

    [unroll]
    for (uint tile_y = 0; tile_y < 4; ++tile_y) {
        [unroll]
        for (uint tile_x = 0; tile_x < 4; ++tile_x) {
            const float2 sample_cell = float2(
                group_thread_id.x + tile_x * 16,
                group_thread_id.y + tile_y * 16);
            const float2 uv = (sample_cell + 0.5f) / 64.0f;
            const float3 color_a = frame_a.SampleLevel(linear_clamp, uv, 0).rgb;
            const float3 color_b = frame_b.SampleLevel(linear_clamp, uv, 0).rgb;
            // Masked UI may legitimately change wholesale (menus, counters)
            // without being a scene cut, so it does not vote here.
            const float sample_weight = AnyMaskEnabled()
                ? 1.0f - MaskAt(sample_cell * (float2(source_size) / 64.0f))
                : 1.0f;
            difference += sample_weight *
                dot(abs(color_a - color_b), float3(0.333333f, 0.333333f, 0.333333f));
            weight += sample_weight;
        }
    }

    scene_difference[flat_index] = difference;
    scene_weight[flat_index] = weight;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint stride = 128; stride > 0; stride >>= 1) {
        if (flat_index < stride) {
            scene_difference[flat_index] += scene_difference[flat_index + stride];
            scene_weight[flat_index] += scene_weight[flat_index + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (flat_index == 0) {
        // Fewer than 1/16 of the probe grid unmasked is not enough evidence
        // to declare a cut, so the ratio is floored at that many samples.
        motion_output[uint2(0, 0)] = float4(
            scene_difference[0] / max(scene_weight[0], 256.0f),
            0.0f,
            0.0f,
            1.0f);
    }
}
)";

constexpr char kMotionPixelShaderSource[] = R"(
Texture2D<float4> previous_frame : register(t0);
Texture2D<float4> current_frame : register(t1);
Texture2D<float4> forward_flow : register(t2);
Texture2D<float4> backward_flow : register(t3);
Texture2D<float4> scene_metrics : register(t4);
// Static UI/HUD coverage: 1 = show the newest real frame unchanged.
Texture2D<float> ui_mask : register(t5);
// Automatic static-overlay detector state at fine-flow resolution.
Texture2D<float4> ui_persistence : register(t6);
SamplerState linear_clamp : register(s0);

cbuffer InterpolationConstants : register(b0) {
    float interpolation_time;
    float2 inverse_source_size;
    float scene_cut_threshold;
    float confidence_floor;
    float static_pixel_low;
    float static_pixel_high;
    float auto_mask_enabled;
    float debug_view;
};

static const float kAutoMaskLow = 0.55f;
static const float kAutoMaskHigh = 0.85f;


static const float kWarpCardinal = -0.40f;

// Confidence-weighted flow magnitude, in source pixels, at which a pixel stops
// counting as static however similar the two frames look at it.
static const float kStaticMotionPixels = 2.0f;

// How hard the fusion leans toward the better-supported warp where the two
// disagree. 1 is a plain weighted average.
static const float kFusionSelectivity = 5.0f;

// Cardinal-spline reconstruction of a warped sample, as five bilinear fetches
// instead of the naive sixteen.
//
// A warp lands on a fractional position, and a plain bilinear fetch there is a
// two-tap box filter: it softens every moving edge. Measured on the pattern's
// three-pixel magenta marker at a 1.44 pixel offset, the marker came back five
// pixels wide holding about 60% of its contrast -- and both sides warp, so the
// fusion applies that softening twice before averaging. Nothing about the flow
// is wrong where this happens. It is the reconstruction filter, and no better
// vector fixes it.
//
// The spline can still overshoot slightly; the caller saturates.
float4 SampleCatmullRom(
    Texture2D<float4> source,
    float2 uv,
    float2 texture_size,
    float2 inverse_texture_size) {
    const float2 sample_position = uv * texture_size;
    const float2 texel_center = floor(sample_position - 0.5f) + 0.5f;
    const float2 fraction = sample_position - texel_center;

    // Cardinal spline weights. kWarpCardinal = -0.5 is Catmull-Rom; softening it
    // trades a little of the recovered sharpness for proportionally less
    // overshoot, and overshoot on a hard edge is a bright or dark outline that
    // tracks the moving object -- a more obvious artifact than the softness this
    // filter exists to remove.
    const float2 squared = fraction * fraction;
    const float2 cubed = squared * fraction;
    const float2 weight0 = kWarpCardinal * (cubed - 2.0f * squared + fraction);
    const float2 weight1 = (kWarpCardinal + 2.0f) * cubed - (kWarpCardinal + 3.0f) * squared + 1.0f;
    const float2 weight2 =
        -(kWarpCardinal + 2.0f) * cubed + (2.0f * kWarpCardinal + 3.0f) * squared -
        kWarpCardinal * fraction;
    const float2 weight3 = -kWarpCardinal * cubed + kWarpCardinal * squared;

    // The middle two taps of each axis are folded into one bilinear fetch placed
    // between them, which is what turns sixteen samples into five.
    const float2 weight12 = weight1 + weight2;
    const float2 offset12 = weight2 / max(weight12, 1e-5f);

    const float2 texel0 = (texel_center - 1.0f) * inverse_texture_size;
    const float2 texel3 = (texel_center + 2.0f) * inverse_texture_size;
    const float2 texel12 = (texel_center + offset12) * inverse_texture_size;

    float4 result = 0.0f;
    result += source.SampleLevel(linear_clamp, float2(texel12.x, texel0.y), 0) * (weight12.x * weight0.y);
    result += source.SampleLevel(linear_clamp, float2(texel0.x, texel12.y), 0) * (weight0.x * weight12.y);
    result += source.SampleLevel(linear_clamp, float2(texel12.x, texel12.y), 0) * (weight12.x * weight12.y);
    result += source.SampleLevel(linear_clamp, float2(texel3.x, texel12.y), 0) * (weight3.x * weight12.y);
    result += source.SampleLevel(linear_clamp, float2(texel12.x, texel3.y), 0) * (weight12.x * weight3.y);
    return result;
}

float IsInside(float2 uv) {
    const float2 valid = step(0.0f, uv) * step(uv, 1.0f);
    return valid.x * valid.y;
}

float FlowConsistency(float2 flow, float2 reverse_flow) {
    const float disagreement = length(flow + reverse_flow);
    const float tolerance = 1.5f + length(flow) * 0.25f;
    return 1.0f - smoothstep(tolerance, tolerance * 2.5f, disagreement);
}

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    const float4 raw_previous = previous_frame.Sample(linear_clamp, uv);
    const float4 raw_current = current_frame.Sample(linear_clamp, uv);
    const float scene_difference = scene_metrics.Load(int3(0, 0, 0)).x;

    if (scene_difference >= scene_cut_threshold) {
        return raw_current;
    }

    // Plain backward warp: the flow sampled at the output position, applied
    // there. The flow field is really indexed by *source* position -- the pixel
    // landing at `uv` at time t started at s = uv - t * flow(s), implicit in s
    // -- so this is an approximation, and it misplaces the sample by roughly
    // t times the local flow gradient.
    //
    // Solving s properly was built and measured three ways and none of them
    // earned their place. Iterating the fixed point undamped is not a
    // contraction: wherever the gradient is steep, which is exactly the motion
    // boundaries the correction is for, it oscillates instead of converging.
    // That failure alternates and so hides easily -- two steps land back near
    // where none started and look fine, while one and three steps each cost
    // about 2.5 dB, and neighbouring pixels settling on different points
    // scatter single wrong pixels through moving objects. Damping it into a
    // genuine contraction converges on an answer *worse* than no correction at
    // all, because at an occlusion boundary the true fixed point lies in the
    // region one of the two frames never saw. Accepting the correction only
    // where it is small, as its own validity test, also measured worse.
    //
    // A converged solution is therefore not what this wants; a flow field
    // resampled to time t by forward splatting is, and that is a different
    // pass, not a smarter sample here. Until then the approximation stands, and
    // it is at least stable, cheap, and never oscillates.
    const float2 previous_uv =
        uv - interpolation_time * forward_flow.Sample(linear_clamp, uv).xy * inverse_source_size;
    const float2 current_uv = uv -
        (1.0f - interpolation_time) * backward_flow.Sample(linear_clamp, uv).xy *
            inverse_source_size;

    const float4 forward = forward_flow.Sample(linear_clamp, previous_uv);
    const float4 backward = backward_flow.Sample(linear_clamp, current_uv);

    const float2 source_size = 1.0f / inverse_source_size;
    const float4 warped_previous = saturate(
        SampleCatmullRom(previous_frame, previous_uv, source_size, inverse_source_size));
    const float4 warped_current = saturate(
        SampleCatmullRom(current_frame, current_uv, source_size, inverse_source_size));

    const float2 forward_endpoint = previous_uv + forward.xy * inverse_source_size;
    const float2 backward_endpoint = current_uv + backward.xy * inverse_source_size;
    const float2 reverse_at_forward = backward_flow.Sample(linear_clamp, forward_endpoint).xy;
    const float2 reverse_at_backward = forward_flow.Sample(linear_clamp, backward_endpoint).xy;

    const float previous_quality =
        forward.w * FlowConsistency(forward.xy, reverse_at_forward) * IsInside(previous_uv);
    const float current_quality =
        backward.w * FlowConsistency(backward.xy, reverse_at_backward) * IsInside(current_uv);

    const float previous_weight = (1.0f - interpolation_time) * previous_quality;
    const float current_weight = interpolation_time * current_quality;

    const float weight_sum = previous_weight + current_weight;
    const float4 temporal_fallback = lerp(raw_previous, raw_current, interpolation_time);
    float4 reconstructed = temporal_fallback;
    if (weight_sum >= confidence_floor) {
        // Where the two warps land on the same colour, averaging them is free
        // noise reduction. Where they disagree, one of them is reading a pixel
        // the other frame never had -- a disocclusion -- and averaging is
        // precisely what turns that into a ghost. Bias hard toward the
        // better-supported side in proportion to how much they disagree.
        const float disagreement = dot(
            abs(warped_previous.rgb - warped_current.rgb),
            float3(0.333333f, 0.333333f, 0.333333f));
        const float selectivity =
            lerp(1.0f, kFusionSelectivity, smoothstep(0.02f, 0.20f, disagreement));
        const float share = previous_weight / max(weight_sum, 0.0001f);
        const float biased = saturate(0.5f + (share - 0.5f) * selectivity);
        reconstructed = lerp(warped_current, warped_previous, biased);
    }

    // Static-pixel protection, gated on the motion field.
    //
    // Comparing the two frames at the same position is a no-motion test, and on
    // its own it is wrong for precisely the case it fires on hardest: an object
    // narrower than its own displacement leaves the pixels it is about to cross
    // *unchanged* between the two source frames. The pattern's magenta marker is
    // three pixels wide and travels 2.9 pixels per frame, so at the midpoint it
    // belongs on pixels that are identical in both sources -- and this test,
    // ungated, concluded nothing had moved there and crossfaded the marker away
    // at exactly the position it should have been drawn. Both warps had it
    // right; the protection discarded them.
    //
    // A pixel is only static if the frames agree AND the motion field says
    // nothing passed through it. Confidence-weighted, so an unconfident vector
    // cannot strip the protection from genuinely still content, which is what
    // the protection is for.
    const float static_difference = dot(
        abs(raw_previous.rgb - raw_current.rgb),
        float3(0.333333f, 0.333333f, 0.333333f));
    const float confident_motion = max(
        length(forward.xy) * forward.w,
        length(backward.xy) * backward.w);
    const float static_protection =
        (1.0f - smoothstep(static_pixel_low, static_pixel_high, static_difference)) *
        (1.0f - saturate(confident_motion / kStaticMotionPixels));
    reconstructed = lerp(reconstructed, temporal_fallback, static_protection);
    reconstructed.a = lerp(raw_previous.a, raw_current.a, interpolation_time);

    // Masked UI/HUD snaps to the newest real frame at source cadence rather
    // than being warped or cross-faded; a changing counter must tick, not blend.
    float mask = ui_mask.Sample(linear_clamp, uv);
    if (auto_mask_enabled > 0.5f) {
        // .y is the dilated detector score.
        mask = max(
            mask,
            smoothstep(kAutoMaskLow, kAutoMaskHigh, ui_persistence.Sample(linear_clamp, uv).y));
    }
    if (debug_view > 0.5f) {
        // Diagnostics replace the frame rather than overlaying it: a legible
        // picture of a flow field cannot share pixels with the scene it
        // describes. Every branch reads intermediates already computed above,
        // so switching a view on costs nothing the normal path was not paying.
        if (debug_view < 1.5f) {
            // Direction as hue, magnitude as brightness. Grey is no motion.
            const float2 motion = forward.xy;
            const float magnitude = length(motion);
            if (magnitude < 0.01f) {
                return float4(0.5f, 0.5f, 0.5f, 1.0f);
            }
            const float2 unit = motion / magnitude;
            const float brightness = saturate(magnitude / 24.0f);
            const float3 hue = saturate(float3(
                0.5f + unit.x * 0.5f,
                0.5f + unit.y * 0.5f,
                0.5f - (unit.x + unit.y) * 0.25f));
            return float4(lerp(float3(0.5f, 0.5f, 0.5f), hue, brightness), 1.0f);
        }
        if (debug_view < 2.5f) {
            // White where the warps were trusted, black where the crossfade
            // fallback took the pixel. A mostly black frame is the signature
            // of an interpolator that has silently become a crossfade.
            const float confidence = saturate(weight_sum);
            return float4(confidence, confidence, confidence, 1.0f);
        }
        // Which safeguard claimed this pixel, one per channel.
        const float below_floor = weight_sum >= confidence_floor ? 0.0f : 1.0f;
        return float4(below_floor, saturate(mask), saturate(static_protection), 1.0f);
    }
    return lerp(reconstructed, raw_current, mask);
}
)";

std::string MotionComputeShaderSource(const bool performance_mode) {
    // Both levers are compile-time, so the fast path carries no dynamic
    // branching and no unused registers. The bytecode cache keys on the source
    // text, so each variant earns its own entry and switching between them
    // recompiles at most once per variant per machine.
    //
    // The coarse search radius was specialised here too, until it started
    // tracking the measured source period; it is a cbuffer value now, for the
    // reason given on `coarse_radius` in the cbuffer. Performance mode still
    // halves it -- through this same flag, inside ResolveCoarseSearchRadius.
    //
    // What stays compile-time is the from-zero pass. Dropping it removes a
    // second 25-candidate local search plus its sub-pixel refinement from every
    // fine cell whose prediction sits outside the refinement window. That pass
    // exists to resolve periodic detail landing in the wrong period, so thin
    // repeating texture is exactly what regresses when it goes -- see the note
    // above it in RefineFlow.
    const char* const prologue = performance_mode
        ? "#define OSSS_REFINE_FROM_ZERO 0\n"
        : "#define OSSS_REFINE_FROM_ZERO 1\n";
    return std::string(prologue) +
        kMotionComputeShaderSourceHead +
        kMotionComputeShaderSourceEstimators +
        kMotionComputeShaderSourceDetectors;
}

UINT DivideRoundUp(const UINT value, const UINT divisor) {
    return (value + divisor - 1) / divisor;
}

} // namespace

void MotionInterpolator::CreateShaders() {
        // Every compute entry point below is a different entry into the *same*
        // translation unit, so compiling them in a loop re-parses and re-optimizes
        // ~35 KB of HLSL once per entry point. That was the single largest cost of
        // starting OSSS: ~12 s measured cold, all of it before the overlay could
        // show a frame. Compile the batch concurrently and let the content-addressed
        // cache in shader_cache.* make every subsequent start free.
        const std::string compute_source = MotionComputeShaderSource(performance_mode_);
        const std::vector<ShaderCompileRequest> requests{
            {compute_source, "BuildLuma", "cs_5_0"},
            {compute_source, "EstimateCoarse", "cs_5_0"},
            {compute_source, "RefineFlow", "cs_5_0"},
            {compute_source, "FilterFlow", "cs_5_0"},
            {compute_source, "MeasureScene", "cs_5_0"},
            {compute_source, "UpdateUiPersistence", "cs_5_0"},
            {kMotionPixelShaderSource, "main", "ps_5_0"},
        };
        const auto compiled = CompileShadersCached(
            requests,
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            "Motion shader compilation failed for ");
        shaders_came_from_cache_ = std::all_of(
            compiled.begin(),
            compiled.end(),
            [](const ShaderCompileResult& result) {
                return result.outcome == ShaderCacheOutcome::hit;
            });
        const auto& luma_bytecode = compiled[0].bytecode;
        const auto& coarse_bytecode = compiled[1].bytecode;
        const auto& refine_bytecode = compiled[2].bytecode;
        const auto& filter_bytecode = compiled[3].bytecode;
        const auto& scene_bytecode = compiled[4].bytecode;
        const auto& persistence_bytecode = compiled[5].bytecode;
        const auto& interpolation_bytecode = compiled[6].bytecode;
        winrt::check_hresult(device_->CreateComputeShader(
            luma_bytecode->GetBufferPointer(),
            luma_bytecode->GetBufferSize(),
            nullptr,
            luma_shader_.put()));
        winrt::check_hresult(device_->CreateComputeShader(
            coarse_bytecode->GetBufferPointer(),
            coarse_bytecode->GetBufferSize(),
            nullptr,
            coarse_shader_.put()));
        winrt::check_hresult(device_->CreateComputeShader(
            refine_bytecode->GetBufferPointer(),
            refine_bytecode->GetBufferSize(),
            nullptr,
            refine_shader_.put()));
        winrt::check_hresult(device_->CreateComputeShader(
            filter_bytecode->GetBufferPointer(),
            filter_bytecode->GetBufferSize(),
            nullptr,
            filter_shader_.put()));
        winrt::check_hresult(device_->CreateComputeShader(
            scene_bytecode->GetBufferPointer(),
            scene_bytecode->GetBufferSize(),
            nullptr,
            scene_shader_.put()));
        winrt::check_hresult(device_->CreateComputeShader(
            persistence_bytecode->GetBufferPointer(),
            persistence_bytecode->GetBufferSize(),
            nullptr,
            persistence_shader_.put()));
        winrt::check_hresult(device_->CreatePixelShader(
            interpolation_bytecode->GetBufferPointer(),
            interpolation_bytecode->GetBufferSize(),
            nullptr,
            interpolation_shader_.put()));
}

MotionInterpolator::MotionInterpolator(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context)
    : flow_timing_(device, context) {
    if (!device || !context) {
        throw std::invalid_argument("MotionInterpolator requires a Direct3D 11 device and context.");
    }
    if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
        throw std::runtime_error("Motion interpolation requires Direct3D feature level 11_0.");
    }

    UINT format_support = 0;
    winrt::check_hresult(device->CheckFormatSupport(kMotionFormat, &format_support));
    constexpr UINT required_support =
        D3D11_FORMAT_SUPPORT_TEXTURE2D |
        D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
        D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    if ((format_support & required_support) != required_support) {
        throw std::runtime_error("The selected GPU cannot expose the required optical-flow texture format.");
    }

    UINT luma_support = 0;
    winrt::check_hresult(device->CheckFormatSupport(kLumaFormat, &luma_support));
    constexpr UINT required_luma_support =
        D3D11_FORMAT_SUPPORT_TEXTURE2D |
        D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
        D3D11_FORMAT_SUPPORT_MIP_AUTOGEN |
        D3D11_FORMAT_SUPPORT_RENDER_TARGET |
        D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    if ((luma_support & required_luma_support) != required_luma_support) {
        throw std::runtime_error("The selected GPU cannot expose the required motion-pyramid texture format.");
    }

    device_.copy_from(device);
    context_.copy_from(context);

    CreateShaders();

    D3D11_SAMPLER_DESC sampler_description{};
    sampler_description.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_description.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_description.MinLOD = 0.0F;
    sampler_description.MaxLOD = D3D11_FLOAT32_MAX;
    winrt::check_hresult(device_->CreateSamplerState(&sampler_description, sampler_.put()));

    D3D11_BUFFER_DESC motion_buffer_description{};
    motion_buffer_description.ByteWidth = sizeof(MotionConstants);
    motion_buffer_description.Usage = D3D11_USAGE_DEFAULT;
    motion_buffer_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    winrt::check_hresult(device_->CreateBuffer(
        &motion_buffer_description,
        nullptr,
        motion_constants_.put()));

    D3D11_BUFFER_DESC interpolation_buffer_description{};
    interpolation_buffer_description.ByteWidth = sizeof(InterpolationConstants);
    interpolation_buffer_description.Usage = D3D11_USAGE_DEFAULT;
    interpolation_buffer_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    winrt::check_hresult(device_->CreateBuffer(
        &interpolation_buffer_description,
        nullptr,
        interpolation_constants_.put()));

    CreateUiMaskTexture();
}

bool MotionInterpolator::PreparePair(
    ID3D11ShaderResourceView* const previous,
    ID3D11ShaderResourceView* const current,
    const UINT source_width,
    const UINT source_height,
    const bool continues_previous_pair) noexcept {
    try {
        if (!previous || !current || source_width == 0 || source_height == 0) {
            throw std::invalid_argument("Motion estimation received an empty frame pair.");
        }
        if (source_width != source_width_ || source_height != source_height_) {
            CreateResources(source_width, source_height);
        }
        // Decided before anything below can clear it: the flag describes the
        // field the *last* pair left, and this pair overwrites that field.
        const bool use_flow_prior =
            temporal_prior_enabled_ && continues_previous_pair && flow_prior_available_;
        flow_prior_available_ = false;
        if (ui_persistence_pending_) {
            // Publish the previous pair's detector result; this pair's flow and
            // fusion both read it, and this pair's own update stays hidden until
            // the next call.
            ui_persistence_index_ = 1 - ui_persistence_index_;
            ui_persistence_pending_ = false;
        }

        MotionConstants constants{
            source_width_,
            source_height_,
            coarse_width_,
            coarse_height_,
            fine_width_,
            fine_height_,
            static_cast<float>(flow_scale_ * 2),
            static_cast<float>(flow_scale_),
            1.0F / static_cast<float>(source_width_),
            1.0F / static_cast<float>(source_height_),
            ui_mask_active_ ? 1U : 0U,
            auto_ui_mask_enabled_ ? 1U : 0U,
            // One coarse texel per search step, so the level the coarse search
            // reads carries only detail that step can actually resolve.
            std::log2(static_cast<float>(flow_scale_ * 2)),
            0.0F,
            // Resolved per pair rather than cached, because the source period
            // moves underneath it and this is the cheapest possible place to
            // read it: the constant buffer is being rebuilt here regardless.
            static_cast<std::int32_t>(
                ResolveCoarseSearchRadius(source_period_seconds_, flow_scale_, performance_mode_)),
            // Forward first; rewritten before the backward coarse dispatch.
            use_flow_prior ? -1.0F : 0.0F,
        };
        context_->UpdateSubresource(motion_constants_.get(), 0, nullptr, &constants, 0, 0);

        // The prior is the previous pair's backward field, read in place: the
        // two coarse dispatches consume it before FilterFlow overwrites it
        // below, and the context executes in order. When no prior is usable
        // the sign is zero and the shader never samples the slot, so binding
        // stale contents is harmless.
        ID3D11ShaderResourceView* const flow_prior_view =
            use_flow_prior ? backward_fine_filtered_.view.get() : nullptr;

        flow_timing_.Begin();
        DispatchLumaPyramids(previous, current);
        DispatchSceneMetrics(previous, current);
        DispatchFlow(
            coarse_shader_.get(),
            previous,
            current,
            previous_luma_.view.get(),
            current_luma_.view.get(),
            nullptr,
            flow_prior_view,
            forward_coarse_.unordered_view.get(),
            coarse_width_,
            coarse_height_);
        if (use_flow_prior) {
            // Same field, opposite sign; see the cbuffer comment. Sixty-four
            // bytes once per pair, and only when there is a prior to sign.
            constants.flow_prior_sign = 1.0F;
            context_->UpdateSubresource(motion_constants_.get(), 0, nullptr, &constants, 0, 0);
        }
        DispatchFlow(
            coarse_shader_.get(),
            current,
            previous,
            current_luma_.view.get(),
            previous_luma_.view.get(),
            nullptr,
            flow_prior_view,
            backward_coarse_.unordered_view.get(),
            coarse_width_,
            coarse_height_);
        DispatchFlow(
            refine_shader_.get(),
            previous,
            current,
            previous_luma_.view.get(),
            current_luma_.view.get(),
            forward_coarse_.view.get(),
            nullptr,
            forward_fine_.unordered_view.get(),
            fine_width_,
            fine_height_);
        DispatchFlow(
            refine_shader_.get(),
            current,
            previous,
            current_luma_.view.get(),
            previous_luma_.view.get(),
            backward_coarse_.view.get(),
            nullptr,
            backward_fine_.unordered_view.get(),
            fine_width_,
            fine_height_);

        DispatchFlowFilter(forward_fine_, forward_fine_filtered_);
        DispatchFlowFilter(backward_fine_, backward_fine_filtered_);
        flow_prior_available_ = true;

        if (auto_ui_mask_enabled_) {
            // Runs after flow so it can use this pair's motion field. The flow
            // above therefore used the previous pair's detector state, which is
            // what keeps the feedback loop acyclic.
            DispatchUiPersistence(previous, current);
        }

        flow_timing_.End();
        ClearComputeBindings();
        ready_ = true;
        last_error_.clear();
        return true;
    } catch (const winrt::hresult_error& error) {
        flow_timing_.Cancel();
        ClearComputeBindings();
        StoreError(winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        flow_timing_.Cancel();
        ClearComputeBindings();
        StoreError(error.what());
    } catch (...) {
        flow_timing_.Cancel();
        ClearComputeBindings();
        StoreError("Unknown Direct3D error during motion estimation.");
    }
    ready_ = false;
    return false;
}

bool MotionInterpolator::BindForDraw(
    ID3D11ShaderResourceView* const previous,
    ID3D11ShaderResourceView* const current,
    const float interpolation_time) noexcept {
    if (!ready_ || !previous || !current) {
        return false;
    }

    try {
        const InterpolationConstants constants{
            std::clamp(interpolation_time, 0.0F, 1.0F),
            1.0F / static_cast<float>(source_width_),
            1.0F / static_cast<float>(source_height_),
            0.28F,
            0.035F,
            0.006F,
            0.030F,
            auto_ui_mask_enabled_ ? 1.0F : 0.0F,
            DebugViewShaderValue(debug_view_),
        };
        context_->UpdateSubresource(interpolation_constants_.get(), 0, nullptr, &constants, 0, 0);

        ID3D11ShaderResourceView* resources[] = {
            previous,
            current,
            forward_fine_filtered_.view.get(),
            backward_fine_filtered_.view.get(),
            scene_metrics_.view.get(),
            ui_mask_view_.get(),
            ui_persistence_[ui_persistence_index_].view.get(),
        };
        ID3D11SamplerState* samplers[] = {sampler_.get()};
        ID3D11Buffer* constant_buffers[] = {interpolation_constants_.get()};
        context_->PSSetShader(interpolation_shader_.get(), nullptr, 0);
        context_->PSSetShaderResources(0, static_cast<UINT>(std::size(resources)), resources);
        context_->PSSetSamplers(0, 1, samplers);
        context_->PSSetConstantBuffers(0, 1, constant_buffers);
        return true;
    } catch (const winrt::hresult_error& error) {
        StoreError(winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        StoreError(error.what());
    } catch (...) {
        StoreError("Unknown Direct3D error while binding motion interpolation.");
    }
    ready_ = false;
    return false;
}

void MotionInterpolator::UnbindAfterDraw() noexcept {
    if (!context_) {
        return;
    }
    ID3D11ShaderResourceView* empty_resources[7]{};
    context_->PSSetShaderResources(0, 7, empty_resources);
}

void MotionInterpolator::Reset() noexcept {
    ready_ = false;
    flow_prior_available_ = false;
}

void MotionInterpolator::PollGpuTimings() noexcept {
    flow_timing_.Poll();
}

bool MotionInterpolator::SetUiMask(std::vector<UiMaskRect> rects) noexcept {
    try {
        std::vector<UiMaskRect> previous_rects = std::move(ui_mask_rects_);
        ui_mask_rects_ = std::move(rects);
        try {
            // CreateUiMaskTexture only publishes its texture on success, so
            // the previous mask stays bound if this throws.
            CreateUiMaskTexture();
        } catch (...) {
            ui_mask_rects_ = std::move(previous_rects);
            throw;
        }
        // Flow computed for the previous pair used the old mask; force a fresh
        // PreparePair before the next draw.
        ready_ = false;
        last_error_.clear();
        return true;
    } catch (const winrt::hresult_error& error) {
        StoreError(winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        StoreError(error.what());
    } catch (...) {
        StoreError("Unknown Direct3D error while creating the UI mask.");
    }
    return false;
}

const std::vector<UiMaskRect>& MotionInterpolator::UiMask() const noexcept {
    return ui_mask_rects_;
}

void MotionInterpolator::SetAutoUiMaskEnabled(const bool enabled) noexcept {
    if (auto_ui_mask_enabled_ == enabled) {
        return;
    }
    auto_ui_mask_enabled_ = enabled;
    // Never carry stale detector state across a toggle: re-enabling starts from
    // nothing and has to earn its persistence again.
    ClearUiPersistence();
    ui_persistence_pending_ = false;
    ready_ = false;
}

bool MotionInterpolator::AutoUiMaskEnabled() const noexcept {
    return auto_ui_mask_enabled_;
}

bool MotionInterpolator::ReadUiPersistence(
    std::vector<UiPersistenceSample>& samples,
    UINT& width,
    UINT& height) const noexcept {
    try {
        // Report the state the current pair actually used, which is the one
        // published at the start of PreparePair.
        const FlowSurface& surface = ui_persistence_[ui_persistence_index_];
        if (!device_ || !context_ || !surface.texture || fine_width_ == 0 || fine_height_ == 0) {
            return false;
        }

        D3D11_TEXTURE2D_DESC description{};
        surface.texture->GetDesc(&description);
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        description.MiscFlags = 0;
        winrt::com_ptr<ID3D11Texture2D> staging;
        winrt::check_hresult(device_->CreateTexture2D(&description, nullptr, staging.put()));
        context_->CopyResource(staging.get(), surface.texture.get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(context_->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        width = fine_width_;
        height = fine_height_;
        samples.assign(static_cast<std::size_t>(width) * height, UiPersistenceSample{});
        for (UINT y = 0; y < height; ++y) {
            const auto* row = static_cast<const std::uint8_t*>(mapped.pData) +
                static_cast<std::size_t>(y) * mapped.RowPitch;
            for (UINT x = 0; x < width; ++x) {
                const auto* texel = reinterpret_cast<const float*>(row) + static_cast<std::size_t>(x) * 4;
                auto& sample = samples[static_cast<std::size_t>(y) * width + x];
                sample.score = texel[0];
                sample.masked_score = texel[1];
                sample.frame_difference = texel[2];
                sample.neighbour_motion = texel[3];
            }
        }
        context_->Unmap(staging.get(), 0);
        return true;
    } catch (...) {
        return false;
    }
}

bool MotionInterpolator::Ready() const noexcept {
    return ready_;
}

bool MotionInterpolator::ShadersCameFromCache() const noexcept {
    return shaders_came_from_cache_;
}

UINT MotionInterpolator::FlowScaleDivisor() const noexcept {
    return flow_scale_;
}

FlowScale MotionInterpolator::FlowScaleSetting() const noexcept {
    return flow_scale_setting_;
}

bool MotionInterpolator::SetPerformanceMode(const bool enabled) noexcept {
    if (enabled == performance_mode_) {
        return true;
    }
    performance_mode_ = enabled;
    // Unlike the flow scale, this changes the shader *source*, so the surfaces
    // survive but every compute shader has to be rebuilt. The on-disk cache
    // makes that free after the first time each variant is seen on a machine.
    try {
        CreateShaders();
    } catch (const std::exception& error) {
        StoreError(error.what());
        ready_ = false;
        return false;
    }
    ready_ = false;
    return true;
}

void MotionInterpolator::SetDebugView(const DebugView view) noexcept {
    debug_view_ = view;
}

DebugView MotionInterpolator::DebugViewSetting() const noexcept {
    return debug_view_;
}

bool MotionInterpolator::PerformanceMode() const noexcept {
    return performance_mode_;
}

void MotionInterpolator::SetTemporalPriorEnabled(const bool enabled) noexcept {
    // Read at the next PreparePair, where it becomes a constant-buffer value;
    // nothing to rebuild and no pair to drop.
    temporal_prior_enabled_ = enabled;
}

bool MotionInterpolator::TemporalPriorEnabled() const noexcept {
    return temporal_prior_enabled_;
}

void MotionInterpolator::SetSourcePeriod(const double seconds) noexcept {
    // No rebuild, no shader recompile, no dropped pair: the radius only bounds a
    // dynamic loop, so the next PreparePair picks the new value up for free.
    // That is what makes it safe to drive this from the presentation loop every
    // iteration, which in turn is what lets the search track a source rate that
    // changes while the game is running.
    source_period_seconds_ = seconds;
}

double MotionInterpolator::SourcePeriod() const noexcept {
    return source_period_seconds_;
}

int MotionInterpolator::CoarseSearchRadius() const noexcept {
    return ResolveCoarseSearchRadius(source_period_seconds_, flow_scale_, performance_mode_);
}

unsigned MotionInterpolator::CoarseSearchPixels() const noexcept {
    return osss::CoarseSearchPixels(CoarseSearchRadius(), flow_scale_);
}

void MotionInterpolator::SetFlowScale(const FlowScale scale) noexcept {
    if (scale == flow_scale_setting_) {
        return;
    }
    flow_scale_setting_ = scale;
    // Same operation the source-size-change path in PreparePair already
    // performs, for the same reason: every flow surface is sized from the
    // resolved divisor, so none of them survive a change to it.
    if (source_width_ != 0 && source_height_ != 0) {
        try {
            CreateResources(source_width_, source_height_);
        } catch (const std::exception& error) {
            StoreError(error.what());
        }
    }
}

const std::string& MotionInterpolator::LastError() const noexcept {
    return last_error_;
}

GpuTimingStatistics MotionInterpolator::FlowGpuTiming() const noexcept {
    return flow_timing_.Statistics();
}

void MotionInterpolator::CreateResources(const UINT source_width, const UINT source_height) {
    source_width_ = source_width;
    source_height_ = source_height;
    const std::uint64_t source_pixels =
        static_cast<std::uint64_t>(source_width) * static_cast<std::uint64_t>(source_height);
    // Source pixels per fine cell come from the FlowScale setting; see
    // src/flow_scale.h for what the divisor trades. A two-pixel grid was
    // built and measured below that floor:
    // it is markedly better on the hard cases -- worst-case occlusion PSNR up
    // 4.4 dB, bad pixels down a quarter, thin-detail flicker down 17% -- but the
    // gain came from the smaller matching patch that fell out of it, not from
    // the finer grid. Decoupling the two (finer grid, patch left at two pixels)
    // recovered the large-object quality and gave most of the hard-case gain
    // back. A one-pixel patch radius is also the configuration most exposed to
    // sensor and compression noise, which this noise-free pattern cannot test,
    // so it is not adopted on synthetic evidence alone. Revisit against captured
    // game frames; the knobs are here and the bench measures it.
    flow_scale_ = ResolveFlowScaleDivisor(flow_scale_setting_, source_pixels);
    fine_width_ = DivideRoundUp(source_width_, flow_scale_);
    fine_height_ = DivideRoundUp(source_height_, flow_scale_);
    coarse_width_ = DivideRoundUp(source_width_, flow_scale_ * 2);
    coarse_height_ = DivideRoundUp(source_height_, flow_scale_ * 2);

    forward_coarse_ = CreateFlowSurface(coarse_width_, coarse_height_);
    backward_coarse_ = CreateFlowSurface(coarse_width_, coarse_height_);
    forward_fine_ = CreateFlowSurface(fine_width_, fine_height_);
    backward_fine_ = CreateFlowSurface(fine_width_, fine_height_);
    forward_fine_filtered_ = CreateFlowSurface(fine_width_, fine_height_);
    backward_fine_filtered_ = CreateFlowSurface(fine_width_, fine_height_);
    scene_metrics_ = CreateFlowSurface(1, 1);
    previous_luma_ = CreateLumaPyramid(source_width_, source_height_);
    current_luma_ = CreateLumaPyramid(source_width_, source_height_);
    ui_persistence_[0] = CreateFlowSurface(fine_width_, fine_height_);
    ui_persistence_[1] = CreateFlowSurface(fine_width_, fine_height_);
    ui_persistence_index_ = 0;
    ui_persistence_pending_ = false;
    ClearUiPersistence();
    CreateUiMaskTexture();
    ready_ = false;
    // Fresh surfaces hold nothing a search should be seeded from.
    flow_prior_available_ = false;
}

void MotionInterpolator::ClearUiPersistence() noexcept {
    if (!context_) {
        return;
    }
    constexpr float cleared[4]{0.0F, 0.0F, 0.0F, 0.0F};
    for (const FlowSurface& surface : ui_persistence_) {
        if (surface.unordered_view) {
            context_->ClearUnorderedAccessViewFloat(surface.unordered_view.get(), cleared);
        }
    }
}

void MotionInterpolator::CreateUiMaskTexture() {
    // Without a source size the regions cannot be resolved yet; a 1x1 zero
    // texture keeps every shader binding valid until CreateResources runs.
    const bool resolvable = source_width_ > 0 && source_height_ > 0 && !ui_mask_rects_.empty();
    std::vector<std::uint8_t> coverage;
    UINT width = 1;
    UINT height = 1;
    bool any_coverage = false;
    if (resolvable) {
        coverage = RasterizeUiMask(ui_mask_rects_, source_width_, source_height_);
        any_coverage = std::any_of(
            coverage.begin(),
            coverage.end(),
            [](const std::uint8_t value) { return value != 0; });
        if (any_coverage) {
            width = source_width_;
            height = source_height_;
        }
    }
    if (!any_coverage) {
        coverage.assign(1, 0);
    }

    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8_UNORM;
    description.SampleDesc = DXGI_SAMPLE_DESC{1, 0};
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initial_data{};
    initial_data.pSysMem = coverage.data();
    initial_data.SysMemPitch = width;

    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::com_ptr<ID3D11ShaderResourceView> view;
    winrt::check_hresult(device_->CreateTexture2D(&description, &initial_data, texture.put()));
    winrt::check_hresult(device_->CreateShaderResourceView(texture.get(), nullptr, view.put()));

    ui_mask_texture_ = std::move(texture);
    ui_mask_view_ = std::move(view);
    ui_mask_active_ = any_coverage;
}

MotionInterpolator::FlowSurface MotionInterpolator::CreateFlowSurface(
    const UINT width,
    const UINT height) const {
    FlowSurface surface;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = kMotionFormat;
    description.SampleDesc = DXGI_SAMPLE_DESC{1, 0};
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    winrt::check_hresult(device_->CreateTexture2D(&description, nullptr, surface.texture.put()));
    winrt::check_hresult(device_->CreateShaderResourceView(
        surface.texture.get(),
        nullptr,
        surface.view.put()));
    winrt::check_hresult(device_->CreateUnorderedAccessView(
        surface.texture.get(),
        nullptr,
        surface.unordered_view.put()));
    return surface;
}

void MotionInterpolator::DispatchFlow(
    ID3D11ComputeShader* const shader,
    ID3D11ShaderResourceView* const frame_a_view,
    ID3D11ShaderResourceView* const frame_b_view,
    ID3D11ShaderResourceView* const luma_a_view,
    ID3D11ShaderResourceView* const luma_b_view,
    ID3D11ShaderResourceView* const coarse_flow_view,
    ID3D11ShaderResourceView* const flow_prior_view,
    ID3D11UnorderedAccessView* const output,
    const UINT width,
    const UINT height) {
    ID3D11ShaderResourceView* resources[] = {
        frame_a_view,
        frame_b_view,
        coarse_flow_view,
        ui_mask_view_.get(),
        // The detector state from the previous pair; this pair's update runs
        // after all four flow dispatches.
        ui_persistence_[ui_persistence_index_].view.get(),
        luma_a_view,
        luma_b_view,
        flow_prior_view,
    };
    ID3D11UnorderedAccessView* outputs[] = {output};
    ID3D11SamplerState* samplers[] = {sampler_.get()};
    ID3D11Buffer* constant_buffers[] = {motion_constants_.get()};
    context_->CSSetShader(shader, nullptr, 0);
    context_->CSSetShaderResources(0, static_cast<UINT>(std::size(resources)), resources);
    context_->CSSetUnorderedAccessViews(0, 1, outputs, nullptr);
    context_->CSSetSamplers(0, 1, samplers);
    context_->CSSetConstantBuffers(0, 1, constant_buffers);
    context_->Dispatch(
        DivideRoundUp(width, kThreadGroupSize),
        DivideRoundUp(height, kThreadGroupSize),
        1);

    ID3D11UnorderedAccessView* empty_output[] = {nullptr};
    ID3D11ShaderResourceView* empty_resources[8]{};
    context_->CSSetUnorderedAccessViews(0, 1, empty_output, nullptr);
    context_->CSSetShaderResources(0, 8, empty_resources);
}

MotionInterpolator::LumaPyramid MotionInterpolator::CreateLumaPyramid(
    const UINT width,
    const UINT height) const {
    LumaPyramid pyramid;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    // 0 means the full chain down to 1x1. GenerateMips fills every level below
    // zero, and the coarse search reads whichever one matches its step.
    description.MipLevels = 0;
    description.ArraySize = 1;
    description.Format = kLumaFormat;
    description.SampleDesc = DXGI_SAMPLE_DESC{1, 0};
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags =
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    description.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    winrt::check_hresult(device_->CreateTexture2D(&description, nullptr, pyramid.texture.put()));
    winrt::check_hresult(device_->CreateShaderResourceView(
        pyramid.texture.get(),
        nullptr,
        pyramid.view.put()));

    D3D11_UNORDERED_ACCESS_VIEW_DESC unordered_description{};
    unordered_description.Format = kLumaFormat;
    unordered_description.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    unordered_description.Texture2D.MipSlice = 0;
    winrt::check_hresult(device_->CreateUnorderedAccessView(
        pyramid.texture.get(),
        &unordered_description,
        pyramid.unordered_view.put()));
    return pyramid;
}

void MotionInterpolator::DispatchLumaPyramids(
    ID3D11ShaderResourceView* const previous,
    ID3D11ShaderResourceView* const current) {
    ID3D11ShaderResourceView* resources[] = {
        previous,
        current,
        nullptr,
        ui_mask_view_.get(),
        ui_persistence_[ui_persistence_index_].view.get(),
        nullptr,
        nullptr,
    };
    ID3D11UnorderedAccessView* outputs[] = {
        nullptr,
        previous_luma_.unordered_view.get(),
        current_luma_.unordered_view.get(),
    };
    ID3D11SamplerState* samplers[] = {sampler_.get()};
    ID3D11Buffer* constant_buffers[] = {motion_constants_.get()};
    context_->CSSetShader(luma_shader_.get(), nullptr, 0);
    context_->CSSetShaderResources(0, static_cast<UINT>(std::size(resources)), resources);
    context_->CSSetUnorderedAccessViews(0, static_cast<UINT>(std::size(outputs)), outputs, nullptr);
    context_->CSSetSamplers(0, 1, samplers);
    context_->CSSetConstantBuffers(0, 1, constant_buffers);
    context_->Dispatch(
        DivideRoundUp(source_width_, kThreadGroupSize),
        DivideRoundUp(source_height_, kThreadGroupSize),
        1);

    // GenerateMips renders into the levels below zero, so the mip-0 unordered
    // access views have to be off the pipeline before it runs.
    ID3D11UnorderedAccessView* empty_outputs[3]{};
    ID3D11ShaderResourceView* empty_resources[8]{};
    context_->CSSetUnorderedAccessViews(0, 3, empty_outputs, nullptr);
    context_->CSSetShaderResources(0, 8, empty_resources);
    context_->GenerateMips(previous_luma_.view.get());
    context_->GenerateMips(current_luma_.view.get());
}

void MotionInterpolator::DispatchFlowFilter(
    const FlowSurface& source,
    const FlowSurface& destination) {
    // The unfiltered field goes in through the coarse-flow slot; FilterFlow
    // reads it with Load, so no sampler state applies.
    ID3D11ShaderResourceView* resources[] = {
        nullptr,
        nullptr,
        source.view.get(),
        ui_mask_view_.get(),
        ui_persistence_[ui_persistence_index_].view.get(),
        previous_luma_.view.get(),
        current_luma_.view.get(),
    };
    ID3D11UnorderedAccessView* outputs[] = {destination.unordered_view.get()};
    ID3D11SamplerState* samplers[] = {sampler_.get()};
    ID3D11Buffer* constant_buffers[] = {motion_constants_.get()};
    context_->CSSetShader(filter_shader_.get(), nullptr, 0);
    context_->CSSetShaderResources(0, static_cast<UINT>(std::size(resources)), resources);
    context_->CSSetUnorderedAccessViews(0, 1, outputs, nullptr);
    context_->CSSetSamplers(0, 1, samplers);
    context_->CSSetConstantBuffers(0, 1, constant_buffers);
    context_->Dispatch(
        DivideRoundUp(fine_width_, kThreadGroupSize),
        DivideRoundUp(fine_height_, kThreadGroupSize),
        1);

    ID3D11UnorderedAccessView* empty_output[] = {nullptr};
    ID3D11ShaderResourceView* empty_resources[8]{};
    context_->CSSetUnorderedAccessViews(0, 1, empty_output, nullptr);
    context_->CSSetShaderResources(0, 8, empty_resources);
}

void MotionInterpolator::DispatchUiPersistence(
    ID3D11ShaderResourceView* const previous,
    ID3D11ShaderResourceView* const current) {
    const std::size_t source_index = ui_persistence_index_;
    const std::size_t destination_index = 1 - source_index;
    // Published at the start of the next pair, not here.

    ID3D11ShaderResourceView* resources[] = {
        previous,
        current,
        // The unfiltered field on purpose. The detector asks only whether the
        // neighbourhood is moving at all, and it takes the maximum over a ring
        // of sparse probes; the outlier filter removes exactly the isolated
        // strong vectors those probes are most likely to land on, which starves
        // the evidence and leaves a real overlay short of arming. Fusion wants
        // the filtered field because a single wrong vector there becomes a
        // visible wrong pixel. Nothing here is warped, so an outlier costs it
        // nothing.
        forward_fine_.view.get(),
        ui_mask_view_.get(),
        ui_persistence_[source_index].view.get(),
        previous_luma_.view.get(),
        current_luma_.view.get(),
    };
    ID3D11UnorderedAccessView* outputs[] = {
        ui_persistence_[destination_index].unordered_view.get(),
    };
    ID3D11SamplerState* samplers[] = {sampler_.get()};
    ID3D11Buffer* constant_buffers[] = {motion_constants_.get()};
    context_->CSSetShader(persistence_shader_.get(), nullptr, 0);
    context_->CSSetShaderResources(0, static_cast<UINT>(std::size(resources)), resources);
    context_->CSSetUnorderedAccessViews(0, 1, outputs, nullptr);
    context_->CSSetSamplers(0, 1, samplers);
    context_->CSSetConstantBuffers(0, 1, constant_buffers);
    context_->Dispatch(
        DivideRoundUp(fine_width_, kThreadGroupSize),
        DivideRoundUp(fine_height_, kThreadGroupSize),
        1);

    ID3D11UnorderedAccessView* empty_output[] = {nullptr};
    ID3D11ShaderResourceView* empty_resources[8]{};
    context_->CSSetUnorderedAccessViews(0, 1, empty_output, nullptr);
    context_->CSSetShaderResources(0, 8, empty_resources);
    ui_persistence_pending_ = true;
}

void MotionInterpolator::DispatchSceneMetrics(
    ID3D11ShaderResourceView* const previous,
    ID3D11ShaderResourceView* const current) {
    ID3D11ShaderResourceView* resources[] = {
        previous,
        current,
        nullptr,
        ui_mask_view_.get(),
        ui_persistence_[ui_persistence_index_].view.get(),
        previous_luma_.view.get(),
        current_luma_.view.get(),
    };
    ID3D11UnorderedAccessView* outputs[] = {scene_metrics_.unordered_view.get()};
    ID3D11SamplerState* samplers[] = {sampler_.get()};
    ID3D11Buffer* constant_buffers[] = {motion_constants_.get()};
    context_->CSSetShader(scene_shader_.get(), nullptr, 0);
    context_->CSSetShaderResources(0, static_cast<UINT>(std::size(resources)), resources);
    context_->CSSetUnorderedAccessViews(0, 1, outputs, nullptr);
    context_->CSSetSamplers(0, 1, samplers);
    context_->CSSetConstantBuffers(0, 1, constant_buffers);
    context_->Dispatch(1, 1, 1);

    ID3D11UnorderedAccessView* empty_output[] = {nullptr};
    ID3D11ShaderResourceView* empty_resources[8]{};
    context_->CSSetUnorderedAccessViews(0, 1, empty_output, nullptr);
    context_->CSSetShaderResources(0, 8, empty_resources);
}

void MotionInterpolator::ClearComputeBindings() noexcept {
    if (!context_) {
        return;
    }
    ID3D11ShaderResourceView* empty_resources[8]{};
    ID3D11UnorderedAccessView* empty_outputs[1]{};
    context_->CSSetShaderResources(0, 8, empty_resources);
    context_->CSSetUnorderedAccessViews(0, 1, empty_outputs, nullptr);
    context_->CSSetShader(nullptr, nullptr, 0);
}

void MotionInterpolator::StoreError(const std::string& message) noexcept {
    try {
        last_error_ = message;
    } catch (...) {
    }
}

} // namespace osss
