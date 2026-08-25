#include "png_writer.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>

namespace osss {
namespace {

// ---- checksums --------------------------------------------------------------

std::array<std::uint32_t, 256> BuildCrcTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < 256; ++index) {
        std::uint32_t value = index;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1U) != 0U ? 0xEDB88320U ^ (value >> 1U) : value >> 1U;
        }
        table[index] = value;
    }
    return table;
}

std::uint32_t Crc32(const std::span<const std::uint8_t> bytes) {
    static const std::array<std::uint32_t, 256> table = BuildCrcTable();
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::uint8_t byte : bytes) {
        crc = table[(crc ^ byte) & 0xFFU] ^ (crc >> 8U);
    }
    return crc ^ 0xFFFFFFFFU;
}

std::uint32_t Adler32(const std::span<const std::uint8_t> bytes) {
    std::uint32_t low = 1;
    std::uint32_t high = 0;
    // 5552 is the longest run that cannot overflow either accumulator.
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t run = std::min<std::size_t>(5552, bytes.size() - offset);
        for (std::size_t index = 0; index < run; ++index) {
            low += bytes[offset + index];
            high += low;
        }
        low %= 65521U;
        high %= 65521U;
        offset += run;
    }
    return (high << 16U) | low;
}

// ---- deflate ----------------------------------------------------------------

// Bits go out least-significant first, which is the packing order in RFC 1951,
// but a Huffman code is defined most-significant first. Two entry points rather
// than one flag with a bool at every call site: getting that order backwards
// produces a stream that still decodes for some inputs, which is the worst way
// for it to be wrong.
class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& output) : output_(output) {}

    void WriteBits(const std::uint32_t value, const int count) {
        for (int index = 0; index < count; ++index) {
            buffer_ |= ((value >> index) & 1U) << held_;
            if (++held_ == 8) {
                output_.push_back(static_cast<std::uint8_t>(buffer_));
                buffer_ = 0;
                held_ = 0;
            }
        }
    }

    void WriteCode(const std::uint32_t code, const int count) {
        for (int index = count - 1; index >= 0; --index) {
            WriteBits((code >> index) & 1U, 1);
        }
    }

    void Flush() {
        if (held_ > 0) {
            output_.push_back(static_cast<std::uint8_t>(buffer_));
            buffer_ = 0;
            held_ = 0;
        }
    }

private:
    std::vector<std::uint8_t>& output_;
    std::uint32_t buffer_ = 0;
    int held_ = 0;
};

// RFC 1951 section 3.2.6: the fixed literal/length alphabet.
void WriteFixedSymbol(BitWriter& writer, const unsigned symbol) {
    if (symbol <= 143) {
        writer.WriteCode(0x30U + symbol, 8);
    } else if (symbol <= 255) {
        writer.WriteCode(0x190U + symbol - 144U, 9);
    } else if (symbol <= 279) {
        writer.WriteCode(symbol - 256U, 7);
    } else {
        writer.WriteCode(0xC0U + symbol - 280U, 8);
    }
}

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

constexpr std::size_t kWindowSize = 32768;
constexpr std::size_t kMinimumMatch = 3;
constexpr std::size_t kMaximumMatch = 258;
constexpr std::size_t kHashBits = 15;
constexpr std::size_t kHashSize = std::size_t{1} << kHashBits;
// Chain depth is the whole speed/ratio dial. 32 costs a few milliseconds on a
// 960x540 frame and lands within a few percent of an exhaustive search on this
// kind of content. A sequence dump writes hundreds of frames, so the difference
// between this and a deeper search is seconds, not minutes.
constexpr int kMaximumChain = 32;

std::uint32_t HashAt(const std::span<const std::uint8_t> data, const std::size_t position) {
    return ((static_cast<std::uint32_t>(data[position]) << 10U) ^
            (static_cast<std::uint32_t>(data[position + 1]) << 5U) ^
            static_cast<std::uint32_t>(data[position + 2])) &
        static_cast<std::uint32_t>(kHashSize - 1);
}

std::vector<std::uint8_t> Deflate(const std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> output;
    output.reserve(data.size() / 2 + 64);
    BitWriter writer(output);
    writer.WriteBits(1, 1); // BFINAL: one block for the whole image.
    writer.WriteBits(1, 2); // BTYPE 01: fixed Huffman.

    const std::size_t size = data.size();
    std::vector<std::int32_t> head(kHashSize, -1);
    std::vector<std::int32_t> previous(size, -1);

    const auto insert = [&](const std::size_t position) {
        if (position + kMinimumMatch > size) {
            return;
        }
        const std::uint32_t hash = HashAt(data, position);
        previous[position] = head[hash];
        head[hash] = static_cast<std::int32_t>(position);
    };

    std::size_t position = 0;
    while (position < size) {
        std::size_t best_length = 0;
        std::size_t best_distance = 0;
        if (position + kMinimumMatch <= size) {
            const std::size_t limit = std::min(kMaximumMatch, size - position);
            std::int32_t candidate = head[HashAt(data, position)];
            int chain = kMaximumChain;
            while (candidate >= 0 && chain-- > 0) {
                const std::size_t start = static_cast<std::size_t>(candidate);
                const std::size_t distance = position - start;
                if (distance == 0 || distance > kWindowSize) {
                    break;
                }
                std::size_t length = 0;
                while (length < limit && data[start + length] == data[position + length]) {
                    ++length;
                }
                if (length > best_length) {
                    best_length = length;
                    best_distance = distance;
                    if (length >= limit) {
                        break;
                    }
                }
                candidate = previous[start];
            }
        }

        if (best_length >= kMinimumMatch) {
            int length_index = 0;
            while (length_index < 28 &&
                   kLengthBase[length_index + 1] <= static_cast<int>(best_length)) {
                ++length_index;
            }
            WriteFixedSymbol(writer, 257U + static_cast<unsigned>(length_index));
            writer.WriteBits(
                static_cast<std::uint32_t>(
                    static_cast<int>(best_length) - kLengthBase[length_index]),
                kLengthExtra[length_index]);

            int distance_index = 0;
            while (distance_index < 29 &&
                   kDistanceBase[distance_index + 1] <= static_cast<int>(best_distance)) {
                ++distance_index;
            }
            writer.WriteCode(static_cast<std::uint32_t>(distance_index), 5);
            writer.WriteBits(
                static_cast<std::uint32_t>(
                    static_cast<int>(best_distance) - kDistanceBase[distance_index]),
                kDistanceExtra[distance_index]);

            for (std::size_t offset = 0; offset < best_length; ++offset) {
                insert(position + offset);
            }
            position += best_length;
        } else {
            WriteFixedSymbol(writer, data[position]);
            insert(position);
            ++position;
        }
    }

    WriteFixedSymbol(writer, 256); // end of block
    writer.Flush();
    return output;
}

std::vector<std::uint8_t> ZlibWrap(const std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> stream;
    // 0x78 0x01: 32 KiB window, deflate, no preset dictionary. 0x7801 is
    // divisible by 31, which is the header check a zlib reader applies.
    stream.push_back(0x78);
    stream.push_back(0x01);
    const std::vector<std::uint8_t> compressed = Deflate(data);
    stream.insert(stream.end(), compressed.begin(), compressed.end());
    const std::uint32_t adler = Adler32(data);
    stream.push_back(static_cast<std::uint8_t>((adler >> 24U) & 0xFFU));
    stream.push_back(static_cast<std::uint8_t>((adler >> 16U) & 0xFFU));
    stream.push_back(static_cast<std::uint8_t>((adler >> 8U) & 0xFFU));
    stream.push_back(static_cast<std::uint8_t>(adler & 0xFFU));
    return stream;
}

// ---- PNG --------------------------------------------------------------------

void AppendBigEndian(std::vector<std::uint8_t>& output, const std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    output.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void AppendChunk(
    std::vector<std::uint8_t>& output,
    const char* const type,
    const std::span<const std::uint8_t> data) {
    AppendBigEndian(output, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> checked(4 + data.size());
    std::transform(type, type + 4, checked.begin(), [](const char byte) {
        return static_cast<std::uint8_t>(byte);
    });
    std::copy(data.begin(), data.end(), checked.begin() + 4);
    output.insert(output.end(), checked.begin(), checked.end());
    AppendBigEndian(output, Crc32(checked));
}

std::uint8_t PaethPredictor(const int left, const int above, const int upper_left) {
    const int estimate = left + above - upper_left;
    const int to_left = std::abs(estimate - left);
    const int to_above = std::abs(estimate - above);
    const int to_upper_left = std::abs(estimate - upper_left);
    if (to_left <= to_above && to_left <= to_upper_left) {
        return static_cast<std::uint8_t>(left);
    }
    if (to_above <= to_upper_left) {
        return static_cast<std::uint8_t>(above);
    }
    return static_cast<std::uint8_t>(upper_left);
}

// Sum of the filtered bytes read as signed magnitudes. This is the heuristic
// the PNG specification suggests: it approximates the entropy of the row
// without compressing it five times.
std::uint64_t FilteredCost(const std::span<const std::uint8_t> row) {
    std::uint64_t total = 0;
    for (const std::uint8_t byte : row) {
        total += byte < 128 ? byte : 256U - byte;
    }
    return total;
}

} // namespace

std::vector<std::uint8_t> EncodePng(
    const std::span<const std::uint32_t> pixels,
    const std::uint32_t width,
    const std::uint32_t height) {
    constexpr std::size_t kBytesPerPixel = 3;
    const std::size_t stride = static_cast<std::size_t>(width) * kBytesPerPixel;

    std::vector<std::uint8_t> current(stride, 0);
    std::vector<std::uint8_t> above(stride, 0);
    std::vector<std::uint8_t> candidate(stride, 0);
    std::vector<std::uint8_t> chosen(stride, 0);

    std::vector<std::uint8_t> raw;
    raw.reserve((stride + 1) * height);

    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t pixel = pixels[static_cast<std::size_t>(y) * width + x];
            current[static_cast<std::size_t>(x) * 3 + 0] =
                static_cast<std::uint8_t>((pixel >> 16U) & 0xFFU);
            current[static_cast<std::size_t>(x) * 3 + 1] =
                static_cast<std::uint8_t>((pixel >> 8U) & 0xFFU);
            current[static_cast<std::size_t>(x) * 3 + 2] =
                static_cast<std::uint8_t>(pixel & 0xFFU);
        }

        std::uint8_t best_filter = 0;
        std::uint64_t best_cost = ~std::uint64_t{0};
        for (std::uint8_t filter = 0; filter < 5; ++filter) {
            for (std::size_t index = 0; index < stride; ++index) {
                const int raw_byte = current[index];
                const int left = index >= kBytesPerPixel ? current[index - kBytesPerPixel] : 0;
                const int up = above[index];
                const int upper_left =
                    index >= kBytesPerPixel ? above[index - kBytesPerPixel] : 0;
                int value = 0;
                switch (filter) {
                case 0:
                    value = raw_byte;
                    break;
                case 1:
                    value = raw_byte - left;
                    break;
                case 2:
                    value = raw_byte - up;
                    break;
                case 3:
                    value = raw_byte - ((left + up) >> 1);
                    break;
                default:
                    value = raw_byte - PaethPredictor(left, up, upper_left);
                    break;
                }
                candidate[index] = static_cast<std::uint8_t>(value & 0xFF);
            }
            const std::uint64_t cost = FilteredCost(candidate);
            if (cost < best_cost) {
                best_cost = cost;
                best_filter = filter;
                chosen.swap(candidate);
            }
        }

        raw.push_back(best_filter);
        raw.insert(raw.end(), chosen.begin(), chosen.end());
        above.swap(current);
    }

    std::vector<std::uint8_t> png{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

    std::vector<std::uint8_t> header;
    AppendBigEndian(header, width);
    AppendBigEndian(header, height);
    header.push_back(8); // bit depth
    header.push_back(2); // colour type 2: truecolour RGB
    header.push_back(0); // compression method 0: deflate
    header.push_back(0); // filter method 0
    header.push_back(0); // no interlace
    AppendChunk(png, "IHDR", header);
    AppendChunk(png, "IDAT", ZlibWrap(raw));
    AppendChunk(png, "IEND", {});
    return png;
}

bool WritePng(
    const std::filesystem::path& path,
    const std::span<const std::uint32_t> pixels,
    const std::uint32_t width,
    const std::uint32_t height,
    std::string& error) {
    if (width == 0 || height == 0) {
        error = "PNG dimensions must both be non-zero.";
        return false;
    }
    if (pixels.size() != static_cast<std::size_t>(width) * height) {
        error = "PNG pixel count does not match its dimensions.";
        return false;
    }
    const std::vector<std::uint8_t> png = EncodePng(pixels, width, height);
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        error = "Could not open the PNG output file.";
        return false;
    }
    output.write(
        reinterpret_cast<const char*>(png.data()),
        static_cast<std::streamsize>(png.size()));
    if (!output) {
        error = "Writing the PNG output file failed.";
        return false;
    }
    error.clear();
    return true;
}

std::string Base64Encode(const std::span<const std::uint8_t> bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t index = 0;
    while (index + 2 < bytes.size()) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(bytes[index]) << 16U) |
            (static_cast<std::uint32_t>(bytes[index + 1]) << 8U) |
            static_cast<std::uint32_t>(bytes[index + 2]);
        encoded.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
        encoded.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
        encoded.push_back(kAlphabet[(triple >> 6U) & 0x3FU]);
        encoded.push_back(kAlphabet[triple & 0x3FU]);
        index += 3;
    }
    const std::size_t remaining = bytes.size() - index;
    if (remaining == 1) {
        const std::uint32_t triple = static_cast<std::uint32_t>(bytes[index]) << 16U;
        encoded.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
        encoded.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
        encoded.push_back('=');
        encoded.push_back('=');
    } else if (remaining == 2) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(bytes[index]) << 16U) |
            (static_cast<std::uint32_t>(bytes[index + 1]) << 8U);
        encoded.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
        encoded.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
        encoded.push_back(kAlphabet[(triple >> 6U) & 0x3FU]);
        encoded.push_back('=');
    }
    return encoded;
}

std::vector<std::uint32_t> DownscalePixels(
    const std::span<const std::uint32_t> pixels,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t divisor,
    std::uint32_t& scaled_width,
    std::uint32_t& scaled_height) {
    if (divisor <= 1) {
        scaled_width = width;
        scaled_height = height;
        return std::vector<std::uint32_t>(pixels.begin(), pixels.end());
    }
    scaled_width = std::max<std::uint32_t>(1, width / divisor);
    scaled_height = std::max<std::uint32_t>(1, height / divisor);
    std::vector<std::uint32_t> scaled(
        static_cast<std::size_t>(scaled_width) * scaled_height, 0xFF000000U);
    for (std::uint32_t y = 0; y < scaled_height; ++y) {
        for (std::uint32_t x = 0; x < scaled_width; ++x) {
            std::uint32_t red = 0;
            std::uint32_t green = 0;
            std::uint32_t blue = 0;
            std::uint32_t counted = 0;
            for (std::uint32_t offset_y = 0; offset_y < divisor; ++offset_y) {
                const std::uint32_t source_y = y * divisor + offset_y;
                if (source_y >= height) {
                    break;
                }
                for (std::uint32_t offset_x = 0; offset_x < divisor; ++offset_x) {
                    const std::uint32_t source_x = x * divisor + offset_x;
                    if (source_x >= width) {
                        break;
                    }
                    const std::uint32_t pixel =
                        pixels[static_cast<std::size_t>(source_y) * width + source_x];
                    red += (pixel >> 16U) & 0xFFU;
                    green += (pixel >> 8U) & 0xFFU;
                    blue += pixel & 0xFFU;
                    ++counted;
                }
            }
            if (counted == 0) {
                continue;
            }
            scaled[static_cast<std::size_t>(y) * scaled_width + x] = 0xFF000000U |
                ((red / counted) << 16U) | ((green / counted) << 8U) | (blue / counted);
        }
    }
    return scaled;
}

} // namespace osss
