#include "shimakaze/snappy_stream.hpp"

#include <snappy.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace shimakaze {
namespace {

constexpr std::size_t max_uncompressed_chunk = 64 * 1024;
constexpr std::array<char, 10> stream_identifier {
    static_cast<char>(0xff), 0x06, 0x00, 0x00, 's', 'N', 'a', 'P', 'p', 'Y',
};

std::uint32_t crc32c(std::span<const char> data)
{
    static constexpr std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> values {};
        for (std::uint32_t i = 0; i < values.size(); ++i) {
            auto crc = i;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 1U) ? (0x82f63b78U ^ (crc >> 1U)) : (crc >> 1U);
            }
            values[i] = crc;
        }
        return values;
    }();

    std::uint32_t crc = 0xffffffffU;
    for (const auto ch : data) {
        const auto byte = static_cast<unsigned char>(ch);
        crc = table[(crc ^ byte) & 0xffU] ^ (crc >> 8U);
    }
    return crc ^ 0xffffffffU;
}

std::uint32_t mask_crc(std::uint32_t crc)
{
    return ((crc >> 15U) | (crc << 17U)) + 0xa282ead8U;
}

void write_le32(char* data, std::uint32_t value)
{
    auto* bytes = reinterpret_cast<unsigned char*>(data);
    bytes[0] = static_cast<unsigned char>(value & 0xffU);
    bytes[1] = static_cast<unsigned char>((value >> 8U) & 0xffU);
    bytes[2] = static_cast<unsigned char>((value >> 16U) & 0xffU);
    bytes[3] = static_cast<unsigned char>((value >> 24U) & 0xffU);
}

std::uint32_t read_le32(const char* data)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

void write_len24(std::vector<char>& chunk, std::size_t length)
{
    chunk[1] = static_cast<char>(length & 0xffU);
    chunk[2] = static_cast<char>((length >> 8U) & 0xffU);
    chunk[3] = static_cast<char>((length >> 16U) & 0xffU);
}

std::size_t read_len24(const char* data)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    return static_cast<std::size_t>(bytes[0]) |
           (static_cast<std::size_t>(bytes[1]) << 8U) |
           (static_cast<std::size_t>(bytes[2]) << 16U);
}

std::vector<char> make_data_chunk(std::span<const char> plain)
{
    std::string compressed;
    snappy::Compress(plain.data(), plain.size(), &compressed);

    const auto use_compressed = compressed.size() < plain.size();
    const auto payload_size = use_compressed ? compressed.size() : plain.size();
    std::vector<char> chunk(4 + 4 + payload_size);
    chunk[0] = use_compressed ? 0x00 : 0x01;
    write_len24(chunk, 4 + payload_size);
    write_le32(chunk.data() + 4, mask_crc(crc32c(plain)));
    if (use_compressed) {
        std::memcpy(chunk.data() + 8, compressed.data(), compressed.size());
    } else if (!plain.empty()) {
        std::memcpy(chunk.data() + 8, plain.data(), plain.size());
    }
    return chunk;
}

} // namespace

std::vector<std::vector<char>> SnappyStreamEncoder::encode(std::span<const char> bytes)
{
    std::vector<std::vector<char>> out;
    if (!wrote_identifier_) {
        out.emplace_back(stream_identifier.begin(), stream_identifier.end());
        wrote_identifier_ = true;
    }

    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto count = std::min(max_uncompressed_chunk, bytes.size() - offset);
        out.push_back(make_data_chunk(std::span<const char>(bytes.data() + offset, count)));
        offset += count;
    }
    return out;
}

std::vector<std::vector<char>> SnappyStreamDecoder::decode(std::span<const char> bytes)
{
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    std::vector<std::vector<char>> out;

    while (buffer_.size() >= 4) {
        const auto chunk_type = static_cast<unsigned char>(buffer_[0]);
        const auto length = read_len24(buffer_.data() + 1);
        if (buffer_.size() < 4 + length) {
            return out;
        }

        const auto* payload = buffer_.data() + 4;
        if (chunk_type == 0xff) {
            if (length != 6 || std::memcmp(payload, "sNaPpY", 6) != 0) {
                throw std::runtime_error("invalid snappy stream identifier");
            }
            saw_identifier_ = true;
        } else if ((chunk_type & 0x80U) != 0) {
            // Skippable chunk.
        } else if (chunk_type == 0x00 || chunk_type == 0x01) {
            if (!saw_identifier_) {
                throw std::runtime_error("snappy data chunk before stream identifier");
            }
            if (length < 4) {
                throw std::runtime_error("invalid snappy data chunk length");
            }
            const auto expected_crc = read_le32(payload);
            const auto data = std::span<const char>(payload + 4, length - 4);
            std::vector<char> plain;
            if (chunk_type == 0x00) {
                std::string uncompressed;
                if (!snappy::Uncompress(data.data(), data.size(), &uncompressed)) {
                    throw std::runtime_error("snappy uncompress failed");
                }
                plain.assign(uncompressed.begin(), uncompressed.end());
            } else {
                plain.assign(data.begin(), data.end());
            }
            if (mask_crc(crc32c(std::span<const char>(plain.data(), plain.size()))) != expected_crc) {
                throw std::runtime_error("snappy crc mismatch");
            }
            out.push_back(std::move(plain));
        } else {
            throw std::runtime_error("unsupported unskippable snappy chunk");
        }

        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(4 + length));
    }

    return out;
}

} // namespace shimakaze
