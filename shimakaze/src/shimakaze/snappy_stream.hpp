#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace shimakaze {

class SnappyStreamEncoder final {
public:
    std::vector<std::vector<char>> encode(std::span<const char> bytes);

private:
    bool wrote_identifier_ = false;
};

class SnappyStreamDecoder final {
public:
    std::vector<std::vector<char>> decode(std::span<const char> bytes);

private:
    std::vector<char> buffer_;
    bool saw_identifier_ = false;
};

} // namespace shimakaze
