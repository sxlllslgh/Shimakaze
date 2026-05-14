#pragma once

#include <optional>
#include <span>
#include <vector>

namespace shimakaze {

std::vector<char> snappy_compress(std::span<const char> payload);
std::optional<std::vector<char>> snappy_uncompress(std::span<const char> payload);

} // namespace shimakaze
