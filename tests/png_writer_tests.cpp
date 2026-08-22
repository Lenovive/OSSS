// Round-trip test for the PNG encoder in src/png_writer.cpp.
//
// The encoder has no library behind it, so "it produced a file" is not evidence
// of anything: a deflate stream with the bit order reversed, a wrong Adler-32,
// or a filter applied without being declared all produce a plausible-looking
// PNG that no decoder will open. The only check worth having is a decode, so
// this file carries a minimal inflater -- fixed-Huffman and stored blocks, the
// two the encoder can emit -- and compares the decoded pixels against the ones
// that went in.
//
// The images below are chosen to exercise the branches that differ: a flat
// region (long matches, one filter), a horizontal gradient (Sub filter), a
// vertical gradient (Up filter), a repeating tile (long-distance matches), and
// incompressible noise (literals only, where the deflate stream is larger than
// its input).

#include "png_writer.h"
#include "test_harness.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using osss::test::Require;

// ---- minimal inflate --------------------------------------------------------

class BitReader {
public:
    explicit BitReader(const std::span<const std::uint8_t> data) : data_(data) {}

    unsigned ReadBit() {
        if (byte_ >= data_.size()) {
            throw std::runtime_error("The deflate stream ended early.");
        }
        const unsigned bit = (data_[byte_] >> bit_) & 1U;
        if (++bit_ == 8) {
            bit_ = 0;
            ++byte_;
        }
        return bit;
    }

    // Deflate packs plain values least-significant bit first.
    unsigned ReadBits(const int count) {
        unsigned value = 0;
        for (int index = 0; index < count; ++index) {
            value |= ReadBit() << index;
        }
        return value;
    }

    // Huffman codes are packed most-significant bit first.
    unsigned ReadCode(const int count) {
        unsigned value = 0;
        for (int index = 0; index < count; ++index) {
            value = (value << 1) | ReadBit();
        }
        return value;
    }

    void AlignToByte() {
        if (bit_ != 0) {
            bit_ = 0;
            ++byte_;
        }
    }

    [[nodiscard]] std::size_t BytePosition() const noexcept {
        return byte_;
    }

    void SkipBytes(const std::size_t count) {
        byte_ += count;
    }

    [[nodiscard]] std::uint8_t ByteAt(const std::size_t index) const {
        if (index >= data_.size()) {
            throw std::runtime_error("The deflate stream ended early.");
        }
        return data_[index];
    }

private:
    std::span<const std::uint8_t> data_;
    std::size_t byte_ = 0;
    int bit_ = 0;
};

constexpr int kLengthBase[] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr int kLengthExtra[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr int kDistanceBase[] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr int kDistanceExtra[] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

unsigned DecodeFixedSymbol(BitReader& reader) {
    // RFC 1951 section 3.2.6 read backwards: 7 bits cover 256-279, 8 bits cover
    // 0-143 and 280-287, 9 bits cover 144-255.
    unsigned value = reader.ReadCode(7);
    if (value <= 0x17U) {
        return value + 256U;
    }
    value = (value << 1) | reader.ReadBit();
    if (value <= 0xBFU) {
        return value - 0x30U;
    }
    if (value <= 0xC7U) {
        return value - 0xC0U + 280U;
    }
    value = (value << 1) | reader.ReadBit();
    return value - 0x190U + 144U;
}

std::vector<std::uint8_t> Inflate(const std::span<const std::uint8_t> stream) {
    Require(stream.size() >= 6, "The zlib stream is too short to hold a header and a checksum.");
    Require(
        (static_cast<unsigned>(stream[0]) << 8U | stream[1]) % 31U == 0U,
        "The zlib header check value is wrong.");
    Require((stream[0] & 0x0FU) == 8U, "The zlib compression method is not deflate.");
    Require((stream[1] & 0x20U) == 0U, "The zlib stream unexpectedly uses a preset dictionary.");

    const std::span<const std::uint8_t> body =
        stream.subspan(2, stream.size() - 6);
    BitReader reader(body);
    std::vector<std::uint8_t> output;

    bool final_block = false;
    while (!final_block) {
        final_block = reader.ReadBit() != 0;
        const unsigned type = reader.ReadBits(2);
        if (type == 0) {
            reader.AlignToByte();
            const std::size_t at = reader.BytePosition();
            const unsigned length =
                static_cast<unsigned>(reader.ByteAt(at)) |
                (static_cast<unsigned>(reader.ByteAt(at + 1)) << 8U);
            const unsigned complement =
                static_cast<unsigned>(reader.ByteAt(at + 2)) |
                (static_cast<unsigned>(reader.ByteAt(at + 3)) << 8U);
            Require(
                (length ^ 0xFFFFU) == complement,
                "A stored deflate block has a mismatched length complement.");
            reader.SkipBytes(4);
            for (unsigned index = 0; index < length; ++index) {
                output.push_back(reader.ByteAt(reader.BytePosition()));
                reader.SkipBytes(1);
            }
            continue;
        }
        Require(type == 1, "Only stored and fixed-Huffman deflate blocks are supported here.");

        while (true) {
            const unsigned symbol = DecodeFixedSymbol(reader);
            if (symbol == 256U) {
                break;
            }
            if (symbol < 256U) {
                output.push_back(static_cast<std::uint8_t>(symbol));
                continue;
            }
            Require(symbol <= 285U, "The deflate stream used an invalid length symbol.");
            const int length_index = static_cast<int>(symbol) - 257;
            const std::size_t length = static_cast<std::size_t>(kLengthBase[length_index]) +
                reader.ReadBits(kLengthExtra[length_index]);
            const unsigned distance_index = reader.ReadCode(5);
            Require(distance_index < 30U, "The deflate stream used an invalid distance symbol.");
            const std::size_t distance =
                static_cast<std::size_t>(kDistanceBase[distance_index]) +
                reader.ReadBits(kDistanceExtra[distance_index]);
            Require(
                distance <= output.size(),
                "The deflate stream referenced a distance before the start of the output.");
            const std::size_t start = output.size() - distance;
            for (std::size_t index = 0; index < length; ++index) {
                output.push_back(output[start + index]);
            }
        }
    }

    // Adler-32 over the decoded bytes, big-endian at the end of the stream.
    std::uint32_t low = 1;
    std::uint32_t high = 0;
    for (const std::uint8_t byte : output) {
        low = (low + byte) % 65521U;
        high = (high + low) % 65521U;
    }
    const std::uint32_t expected = (high << 16U) | low;
    const std::uint32_t stored =
        (static_cast<std::uint32_t>(stream[stream.size() - 4]) << 24U) |
        (static_cast<std::uint32_t>(stream[stream.size() - 3]) << 16U) |
        (static_cast<std::uint32_t>(stream[stream.size() - 2]) << 8U) |
        static_cast<std::uint32_t>(stream[stream.size() - 1]);
    Require(expected == stored, "The zlib Adler-32 checksum does not match the decoded data.");
    return output;
}

// ---- minimal PNG parse ------------------------------------------------------

std::uint32_t Crc32(const std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0U ? 0xEDB88320U ^ (crc >> 1U) : crc >> 1U;
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

std::uint32_t BigEndianAt(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
        static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint8_t PaethPredictor(const int left, const int above, const int upper_left) {
    const int estimate = left + above - upper_left;
    const int to_left = estimate > left ? estimate - left : left - estimate;
    const int to_above = estimate > above ? estimate - above : above - estimate;
    const int to_upper_left =
        estimate > upper_left ? estimate - upper_left : upper_left - estimate;
    if (to_left <= to_above && to_left <= to_upper_left) {
        return static_cast<std::uint8_t>(left);
    }
    if (to_above <= to_upper_left) {
        return static_cast<std::uint8_t>(above);
    }
    return static_cast<std::uint8_t>(upper_left);
}

std::vector<std::uint32_t> DecodePng(
    const std::span<const std::uint8_t> png,
    std::uint32_t& width,
    std::uint32_t& height) {
    static constexpr std::uint8_t kSignature[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    Require(png.size() > sizeof(kSignature), "The PNG is too short to hold a signature.");
    for (std::size_t index = 0; index < sizeof(kSignature); ++index) {
        Require(png[index] == kSignature[index], "The PNG signature is wrong.");
    }

    std::vector<std::uint8_t> compressed;
    bool saw_header = false;
    bool saw_end = false;
    std::size_t offset = sizeof(kSignature);
    while (offset + 12 <= png.size()) {
        const std::uint32_t length = BigEndianAt(png, offset);
        const std::string type(
            reinterpret_cast<const char*>(png.data()) + offset + 4, 4);
        Require(
            offset + 12 + length <= png.size(),
            "A PNG chunk claims to be longer than the file.");
        const auto payload = png.subspan(offset + 8, length);
        const auto checked = png.subspan(offset + 4, length + 4);
        Require(
            Crc32(checked) == BigEndianAt(png, offset + 8 + length),
            "A PNG chunk CRC is wrong: " + type);

        if (type == "IHDR") {
            Require(length == 13, "IHDR must be 13 bytes.");
            width = BigEndianAt(payload, 0);
            height = BigEndianAt(payload, 4);
            Require(payload[8] == 8, "Expected an 8-bit depth.");
            Require(payload[9] == 2, "Expected colour type 2 (truecolour).");
            Require(payload[10] == 0, "Expected deflate compression.");
            Require(payload[11] == 0, "Expected filter method 0.");
            Require(payload[12] == 0, "Expected a non-interlaced image.");
            saw_header = true;
        } else if (type == "IDAT") {
            Require(saw_header, "IDAT appeared before IHDR.");
            compressed.insert(compressed.end(), payload.begin(), payload.end());
        } else if (type == "IEND") {
            saw_end = true;
        }
        offset += 12 + length;
    }
    Require(saw_header, "The PNG has no IHDR chunk.");
    Require(saw_end, "The PNG has no IEND chunk.");
    Require(offset == png.size(), "The PNG has trailing bytes after its last chunk.");

    const std::vector<std::uint8_t> raw = Inflate(compressed);
    constexpr std::size_t kBytesPerPixel = 3;
    const std::size_t stride = static_cast<std::size_t>(width) * kBytesPerPixel;
    Require(
        raw.size() == (stride + 1) * height,
        "The decompressed image is not one filter byte plus one row per scanline.");

    std::vector<std::uint8_t> previous(stride, 0);
    std::vector<std::uint8_t> current(stride, 0);
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height, 0xFF000000U);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::size_t row_start = static_cast<std::size_t>(y) * (stride + 1);
        const std::uint8_t filter = raw[row_start];
        Require(filter <= 4, "A PNG scanline declared an unknown filter type.");
        for (std::size_t index = 0; index < stride; ++index) {
            const int encoded = raw[row_start + 1 + index];
            const int left = index >= kBytesPerPixel ? current[index - kBytesPerPixel] : 0;
            const int up = previous[index];
            const int upper_left = index >= kBytesPerPixel ? previous[index - kBytesPerPixel] : 0;
            int value = encoded;
            switch (filter) {
            case 1:
                value = encoded + left;
                break;
            case 2:
                value = encoded + up;
                break;
            case 3:
                value = encoded + ((left + up) >> 1);
                break;
            case 4:
                value = encoded + PaethPredictor(left, up, upper_left);
                break;
            default:
                break;
            }
            current[index] = static_cast<std::uint8_t>(value & 0xFF);
        }
        for (std::uint32_t x = 0; x < width; ++x) {
            pixels[static_cast<std::size_t>(y) * width + x] = 0xFF000000U |
                (static_cast<std::uint32_t>(current[static_cast<std::size_t>(x) * 3 + 0]) << 16U) |
                (static_cast<std::uint32_t>(current[static_cast<std::size_t>(x) * 3 + 1]) << 8U) |
                static_cast<std::uint32_t>(current[static_cast<std::size_t>(x) * 3 + 2]);
        }
        previous.swap(current);
    }
    return pixels;
}

// ---- images that exercise different encoder branches ------------------------

std::vector<std::uint32_t> MakeImage(
    const std::string& kind,
    const std::uint32_t width,
    const std::uint32_t height) {
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height, 0xFF000000U);
    std::mt19937 generator(1234);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint32_t red = 0;
            std::uint32_t green = 0;
            std::uint32_t blue = 0;
            if (kind == "flat") {
                red = 0x20;
                green = 0x90;
                blue = 0xC0;
            } else if (kind == "horizontal") {
                red = (x * 255) / std::max<std::uint32_t>(width - 1, 1);
                green = red;
                blue = 255 - red;
            } else if (kind == "vertical") {
                green = (y * 255) / std::max<std::uint32_t>(height - 1, 1);
                red = 255 - green;
                blue = 128;
            } else if (kind == "tiled") {
                red = ((x / 8 + y / 8) % 2) != 0 ? 240 : 16;
                green = (x % 8) * 30;
                blue = (y % 8) * 30;
            } else {
                red = generator() & 0xFFU;
                green = generator() & 0xFFU;
                blue = generator() & 0xFFU;
            }
            pixels[static_cast<std::size_t>(y) * width + x] =
                0xFF000000U | (red << 16U) | (green << 8U) | blue;
        }
    }
    return pixels;
}

void TestRoundTrip() {
    // Odd dimensions on purpose: an encoder that assumes a multiple of the
    // filter unit or of the base64 triple only fails on sizes like these.
    const std::uint32_t width = 61;
    const std::uint32_t height = 37;
    for (const std::string kind : {"flat", "horizontal", "vertical", "tiled", "noise"}) {
        const auto original = MakeImage(kind, width, height);
        const auto png = osss::EncodePng(original, width, height);
        std::uint32_t decoded_width = 0;
        std::uint32_t decoded_height = 0;
        const auto decoded = DecodePng(png, decoded_width, decoded_height);
        Require(decoded_width == width, "Decoded width differs for the " + kind + " image.");
        Require(decoded_height == height, "Decoded height differs for the " + kind + " image.");
        Require(
            decoded.size() == original.size(),
            "Decoded pixel count differs for the " + kind + " image.");
        for (std::size_t index = 0; index < original.size(); ++index) {
            Require(
                decoded[index] == original[index],
                "Pixel " + std::to_string(index) + " changed in the " + kind + " round trip.");
        }
    }
}

// A single row and a single column are the two cases where a filter reads a row
// or a column that does not exist.
void TestDegenerateDimensions() {
    for (const auto [width, height] :
         {std::pair<std::uint32_t, std::uint32_t>{1, 1},
          std::pair<std::uint32_t, std::uint32_t>{1, 64},
          std::pair<std::uint32_t, std::uint32_t>{64, 1}}) {
        const auto original = MakeImage("tiled", width, height);
        const auto png = osss::EncodePng(original, width, height);
        std::uint32_t decoded_width = 0;
        std::uint32_t decoded_height = 0;
        const auto decoded = DecodePng(png, decoded_width, decoded_height);
        Require(decoded_width == width && decoded_height == height, "Degenerate size changed.");
        for (std::size_t index = 0; index < original.size(); ++index) {
            Require(decoded[index] == original[index], "Degenerate image pixel changed.");
        }
    }
}

// Alpha is dropped by design; a source pixel with a transparent alpha must
// still round-trip its colour channels.
void TestAlphaIsIgnored() {
    const std::uint32_t width = 9;
    const std::uint32_t height = 4;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height);
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        pixels[index] = (static_cast<std::uint32_t>(index % 256) << 24U) | 0x00336699U;
    }
    const auto png = osss::EncodePng(pixels, width, height);
    std::uint32_t decoded_width = 0;
    std::uint32_t decoded_height = 0;
    const auto decoded = DecodePng(png, decoded_width, decoded_height);
    for (const std::uint32_t pixel : decoded) {
        Require(pixel == 0xFF336699U, "Colour channels did not survive a varying alpha.");
    }
}

void TestCompressionIsUseful() {
    // Not a ratio guarantee, a sanity floor: a flat image that does not
    // compress means the match finder is not finding matches at all, which is
    // the failure mode where the encoder still produces a valid file.
    const std::uint32_t width = 256;
    const std::uint32_t height = 256;
    const auto flat = MakeImage("flat", width, height);
    const auto encoded = osss::EncodePng(flat, width, height);
    const std::size_t uncompressed = static_cast<std::size_t>(width) * height * 3;
    Require(
        encoded.size() < uncompressed / 50,
        "A flat 256x256 image should compress by far more than 50x; got " +
            std::to_string(encoded.size()) + " bytes.");
}

void TestBase64() {
    const auto encode = [](const std::string& text) {
        std::vector<std::uint8_t> bytes(text.begin(), text.end());
        return osss::Base64Encode(bytes);
    };
    // RFC 4648 test vectors: they pin the padding for all three length classes.
    Require(encode("") == "", "Base64 of the empty string must be empty.");
    Require(encode("f") == "Zg==", "Base64 of one byte is wrong.");
    Require(encode("fo") == "Zm8=", "Base64 of two bytes is wrong.");
    Require(encode("foo") == "Zm9v", "Base64 of three bytes is wrong.");
    Require(encode("foob") == "Zm9vYg==", "Base64 of four bytes is wrong.");
    Require(encode("fooba") == "Zm9vYmE=", "Base64 of five bytes is wrong.");
    Require(encode("foobar") == "Zm9vYmFy", "Base64 of six bytes is wrong.");
}

void TestDownscale() {
    const std::uint32_t width = 8;
    const std::uint32_t height = 4;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height, 0xFF000000U);
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        // Left half black, right half white: a 2x box average of either half
        // must stay saturated, and the boundary must land on a pixel edge.
        pixels[index] = (index % width) < width / 2 ? 0xFF000000U : 0xFFFFFFFFU;
    }

    std::uint32_t scaled_width = 0;
    std::uint32_t scaled_height = 0;
    const auto same =
        osss::DownscalePixels(pixels, width, height, 1, scaled_width, scaled_height);
    Require(scaled_width == width && scaled_height == height, "Divisor 1 must not resize.");
    Require(same == pixels, "Divisor 1 must return the pixels unchanged.");

    const auto half =
        osss::DownscalePixels(pixels, width, height, 2, scaled_width, scaled_height);
    Require(scaled_width == 4 && scaled_height == 2, "A divisor of 2 must halve both axes.");
    Require(half.size() == 8, "A divisor of 2 must produce width/2 * height/2 pixels.");
    for (std::uint32_t y = 0; y < scaled_height; ++y) {
        for (std::uint32_t x = 0; x < scaled_width; ++x) {
            const std::uint32_t pixel = half[static_cast<std::size_t>(y) * scaled_width + x];
            Require(
                pixel == (x < 2 ? 0xFF000000U : 0xFFFFFFFFU),
                "A box average inside one flat half must stay that colour.");
        }
    }

    // The image is not a multiple of the divisor: the partial edge cells are
    // averaged over what exists rather than reading past the end.
    const auto third =
        osss::DownscalePixels(pixels, width, height, 3, scaled_width, scaled_height);
    Require(scaled_width == 2 && scaled_height == 1, "A divisor of 3 must floor both axes.");
    Require(third.size() == 2, "A divisor of 3 must produce two pixels here.");
}

} // namespace

int main() {
    try {
        TestRoundTrip();
        TestDegenerateDimensions();
        TestAlphaIsIgnored();
        TestCompressionIsUseful();
        TestBase64();
        TestDownscale();
        std::cout << "OSSS PNG encode/decode round-trip, base64, and downscale tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
    }
    return EXIT_FAILURE;
}
