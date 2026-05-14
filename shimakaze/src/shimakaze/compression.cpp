#include "shimakaze/compression.hpp"

#include <snappy.h>

#include <string>

namespace shimakaze {

std::vector<char> snappy_compress(std::span<const char> payload)
{
    std::string compressed;
    snappy::Compress(payload.data(), payload.size(), &compressed);
    return {compressed.begin(), compressed.end()};
}

std::optional<std::vector<char>> snappy_uncompress(std::span<const char> payload)
{
    std::string uncompressed;
    if (!snappy::Uncompress(payload.data(), payload.size(), &uncompressed)) {
        return std::nullopt;
    }
    return std::vector<char>(uncompressed.begin(), uncompressed.end());
}

} // namespace shimakaze
