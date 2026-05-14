#pragma once

#include "shimakaze/config.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace shimakaze {

inline constexpr std::size_t fec_header_size = 6;
inline constexpr std::size_t fec_header_size_plus_size = 8;
inline constexpr std::uint16_t fec_type_data = 0x00f1;
inline constexpr std::uint16_t fec_type_parity = 0x00f2;
inline constexpr std::uint16_t fec_type_oob = 0x00f3;

std::optional<std::size_t> fec_kcp_payload_offset(std::span<const char> packet);

class FecCodec final {
public:
    explicit FecCodec(const BaseConfig& config);
    ~FecCodec();

    bool enabled() const noexcept { return enabled_; }
    std::size_t overhead() const noexcept { return enabled_ ? fec_header_size_plus_size : 0; }

    std::vector<std::vector<char>> encode(std::span<const char> kcp_packet);
    std::vector<std::vector<char>> decode(std::span<const char> packet);

private:
    class ReedSolomon;

    struct ShardSet {
        std::vector<std::vector<char>> shards;
        std::vector<bool> present;
        std::vector<bool> data_shard;
    };

    static std::uint16_t read_le16(const char* data);
    static std::uint32_t read_le32(const char* data);
    static void write_le16(char* data, std::uint16_t value);
    static void write_le32(char* data, std::uint32_t value);
    static std::uint16_t packet_flag(std::span<const char> packet);
    static std::uint32_t packet_seq(std::span<const char> packet);

    void seal_packet(std::vector<char>& packet, std::uint16_t type);
    void skip_parity();
    void discard_old_shards(std::uint32_t newest_shard_id);

    int data_shards_ = 0;
    int parity_shards_ = 0;
    int shard_size_ = 0;
    bool enabled_ = false;
    std::uint32_t paws_ = 0;
    std::uint32_t next_seq_ = 0;
    int shard_count_ = 0;
    std::size_t max_shard_len_ = 0;
    std::int64_t latest_packet_ms_ = 0;
    std::uint32_t newest_shard_id_ = 0;
    std::vector<std::vector<char>> encode_shards_;
    std::unordered_map<std::uint32_t, ShardSet> decode_sets_;
    std::unique_ptr<ReedSolomon> rs_;
};

} // namespace shimakaze
