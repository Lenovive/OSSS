#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace osss {

// 8-bit truecolour PNG encoding, and the two helpers a frame viewer needs
// alongside it.
//
// This exists because every image this repo produced was a binary P6 PPM, which
// nothing on a normal desktop opens and no agent reading this codebase can look
// at. Every motion defect found so far was diagnosed by a human editing the
// fusion shader and watching the screen (see src/debug_view.h); the dumps were
// numbers, not pictures. PNG is the format that makes a dumped frame something
// anyone -- or anything -- can actually open.
//
// Dependency-free on purpose: no zlib, no image library. The deflate stream is
// fixed-Huffman with a hash-chain match finder, which on synthetic pattern
// content lands within a few percent of a real zlib at level 6 and is a couple
// of hundred lines rather than a third-party dependency. See png_writer.cpp.

// Encodes 0xAARRGGBB pixels as a truecolour (colour type 2) PNG. Alpha is
// dropped: every consumer here renders opaque frames, and a 25% smaller file
// matters when a sequence dump writes hundreds of them.
[[nodiscard]] std::vector<std::uint8_t> EncodePng(
    std::span<const std::uint32_t> pixels,
    std::uint32_t width,
    std::uint32_t height);

bool WritePng(
    const std::filesystem::path& path,
    std::span<const std::uint32_t> pixels,
    std::uint32_t width,
    std::uint32_t height,
    std::string& error);

// Standard base64, no line breaks -- the form a `data:image/png;base64,` URI
// wants. Used by the embedded single-file frame viewer.
[[nodiscard]] std::string Base64Encode(std::span<const std::uint8_t> bytes);

// Integer box downscale. A divisor of 1 returns a copy. Used only to keep an
// embedded viewer under a size ceiling; nothing scored ever goes through it.
[[nodiscard]] std::vector<std::uint32_t> DownscalePixels(
    std::span<const std::uint32_t> pixels,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t divisor,
    std::uint32_t& scaled_width,
    std::uint32_t& scaled_height);

} // namespace osss
