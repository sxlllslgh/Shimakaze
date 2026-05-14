#include "shimakaze/fec.hpp"

#include "shimakaze/stats.hpp"
#include "shimakaze/utils.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace shimakaze {
namespace {

constexpr std::int64_t max_fec_encode_latency_ms = 500;
constexpr std::uint32_t max_shard_sets = 3;

std::int32_t timediff(std::uint32_t later, std::uint32_t earlier)
{
    return static_cast<std::int32_t>(later - earlier);
}

std::uint16_t read_le16_raw(const char* data)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::uint8_t gf_mul(std::uint8_t a, std::uint8_t b)
{
    std::uint8_t result = 0;
    while (b != 0) {
        if ((b & 1U) != 0) {
            result ^= a;
        }
        const auto carry = (a & 0x80U) != 0;
        a = static_cast<std::uint8_t>(a << 1U);
        if (carry) {
            a ^= 0x1dU;
        }
        b = static_cast<std::uint8_t>(b >> 1U);
    }
    return result;
}

std::uint8_t gf_pow(std::uint8_t value, int power)
{
    std::uint8_t result = 1;
    for (int i = 0; i < power; ++i) {
        result = gf_mul(result, value);
    }
    return result;
}

std::uint8_t gf_inv(std::uint8_t value)
{
    if (value == 0) {
        throw std::runtime_error("cannot invert zero in GF(256)");
    }
    return gf_pow(value, 254);
}

using Matrix = std::vector<std::vector<std::uint8_t>>;

Matrix multiply_matrix(const Matrix& left, const Matrix& right)
{
    Matrix out(left.size(), std::vector<std::uint8_t>(right.front().size(), 0));
    for (std::size_t r = 0; r < left.size(); ++r) {
        for (std::size_t c = 0; c < right.front().size(); ++c) {
            std::uint8_t value = 0;
            for (std::size_t i = 0; i < right.size(); ++i) {
                value ^= gf_mul(left[r][i], right[i][c]);
            }
            out[r][c] = value;
        }
    }
    return out;
}

Matrix invert_matrix(const Matrix& input)
{
    const auto size = input.size();
    Matrix work(size, std::vector<std::uint8_t>(size * 2, 0));
    for (std::size_t r = 0; r < size; ++r) {
        for (std::size_t c = 0; c < size; ++c) {
            work[r][c] = input[r][c];
        }
        work[r][size + r] = 1;
    }

    for (std::size_t r = 0; r < size; ++r) {
        if (work[r][r] == 0) {
            std::size_t row_below = r + 1;
            for (; row_below < size; ++row_below) {
                if (work[row_below][r] != 0) {
                    std::swap(work[r], work[row_below]);
                    break;
                }
            }
        }
        if (work[r][r] == 0) {
            throw std::runtime_error("singular Reed-Solomon matrix");
        }
        if (work[r][r] != 1) {
            const auto scale = gf_inv(work[r][r]);
            for (std::size_t c = 0; c < size * 2; ++c) {
                work[r][c] = gf_mul(work[r][c], scale);
            }
        }
        for (std::size_t row = r + 1; row < size; ++row) {
            if (work[row][r] == 0) {
                continue;
            }
            const auto scale = work[row][r];
            for (std::size_t c = 0; c < size * 2; ++c) {
                work[row][c] ^= gf_mul(scale, work[r][c]);
            }
        }
    }

    for (std::size_t d = 0; d < size; ++d) {
        for (std::size_t row = 0; row < d; ++row) {
            if (work[row][d] == 0) {
                continue;
            }
            const auto scale = work[row][d];
            for (std::size_t c = 0; c < size * 2; ++c) {
                work[row][c] ^= gf_mul(scale, work[d][c]);
            }
        }
    }

    Matrix out(size, std::vector<std::uint8_t>(size, 0));
    for (std::size_t r = 0; r < size; ++r) {
        std::copy_n(work[r].begin() + static_cast<std::ptrdiff_t>(size), size, out[r].begin());
    }
    return out;
}

Matrix build_rs_matrix(int data_shards, int total_shards)
{
    Matrix vandermonde(static_cast<std::size_t>(total_shards),
                       std::vector<std::uint8_t>(static_cast<std::size_t>(data_shards), 0));
    for (int r = 0; r < total_shards; ++r) {
        for (int c = 0; c < data_shards; ++c) {
            vandermonde[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] =
                gf_pow(static_cast<std::uint8_t>(r), c);
        }
    }

    Matrix top(static_cast<std::size_t>(data_shards),
               std::vector<std::uint8_t>(static_cast<std::size_t>(data_shards), 0));
    for (int r = 0; r < data_shards; ++r) {
        std::copy_n(vandermonde[static_cast<std::size_t>(r)].begin(), data_shards, top[static_cast<std::size_t>(r)].begin());
    }
    return multiply_matrix(vandermonde, invert_matrix(top));
}

} // namespace

class FecCodec::ReedSolomon final {
public:
    ReedSolomon(int data_shards, int parity_shards)
        : data_shards_(data_shards)
        , parity_shards_(parity_shards)
        , total_shards_(data_shards + parity_shards)
        , matrix_(build_rs_matrix(data_shards, data_shards + parity_shards))
    {
    }

    void encode(const std::vector<std::vector<char>>& data, std::vector<std::vector<char>>& parity) const
    {
        for (int p = 0; p < parity_shards_; ++p) {
            auto& out = parity[static_cast<std::size_t>(p)];
            std::ranges::fill(out, 0);
            const auto& row = matrix_[static_cast<std::size_t>(data_shards_ + p)];
            for (int d = 0; d < data_shards_; ++d) {
                const auto coefficient = row[static_cast<std::size_t>(d)];
                if (coefficient == 0) {
                    continue;
                }
                const auto& shard = data[static_cast<std::size_t>(d)];
                for (std::size_t i = 0; i < shard.size(); ++i) {
                    out[i] ^= static_cast<char>(gf_mul(coefficient, static_cast<std::uint8_t>(shard[i])));
                }
            }
        }
    }

    bool reconstruct_data(std::vector<std::vector<char>>& shards, const std::vector<bool>& present) const
    {
        std::vector<int> valid_indices;
        valid_indices.reserve(static_cast<std::size_t>(data_shards_));
        for (int i = 0; i < total_shards_ && static_cast<int>(valid_indices.size()) < data_shards_; ++i) {
            if (present[static_cast<std::size_t>(i)]) {
                valid_indices.push_back(i);
            }
        }
        if (static_cast<int>(valid_indices.size()) < data_shards_) {
            return false;
        }

        Matrix sub_matrix(static_cast<std::size_t>(data_shards_),
                          std::vector<std::uint8_t>(static_cast<std::size_t>(data_shards_), 0));
        for (int r = 0; r < data_shards_; ++r) {
            sub_matrix[static_cast<std::size_t>(r)] = matrix_[static_cast<std::size_t>(valid_indices[static_cast<std::size_t>(r)])];
        }
        const auto data_decode = invert_matrix(sub_matrix);

        const auto shard_len = shards[static_cast<std::size_t>(valid_indices.front())].size();
        for (int missing = 0; missing < data_shards_; ++missing) {
            if (present[static_cast<std::size_t>(missing)]) {
                continue;
            }
            std::vector<char> recovered(shard_len, 0);
            const auto& row = data_decode[static_cast<std::size_t>(missing)];
            for (int r = 0; r < data_shards_; ++r) {
                const auto coefficient = row[static_cast<std::size_t>(r)];
                if (coefficient == 0) {
                    continue;
                }
                const auto& shard = shards[static_cast<std::size_t>(valid_indices[static_cast<std::size_t>(r)])];
                for (std::size_t i = 0; i < shard_len; ++i) {
                    recovered[i] ^= static_cast<char>(gf_mul(coefficient, static_cast<std::uint8_t>(shard[i])));
                }
            }
            shards[static_cast<std::size_t>(missing)] = std::move(recovered);
        }
        return true;
    }

private:
    int data_shards_ = 0;
    int parity_shards_ = 0;
    int total_shards_ = 0;
    Matrix matrix_;
};

std::optional<std::size_t> fec_kcp_payload_offset(std::span<const char> packet)
{
    if (packet.size() < fec_header_size_plus_size) {
        return 0;
    }
    const auto flag = read_le16_raw(packet.data() + 4);
    if (flag == fec_type_data || flag == fec_type_oob) {
        return fec_header_size_plus_size;
    }
    if (flag == fec_type_parity) {
        return std::nullopt;
    }
    return 0;
}

FecCodec::FecCodec(const BaseConfig& config)
    : data_shards_(config.datashard)
    , parity_shards_(config.parityshard)
    , shard_size_(config.datashard + config.parityshard)
    , enabled_(config.datashard > 0 && config.parityshard > 0 && config.datashard + config.parityshard <= 256)
{
    if (!enabled_) {
        return;
    }

    paws_ = 0xffffffffU / static_cast<std::uint32_t>(shard_size_) * static_cast<std::uint32_t>(shard_size_);
    encode_shards_.resize(static_cast<std::size_t>(data_shards_));
    rs_ = std::make_unique<ReedSolomon>(data_shards_, parity_shards_);
}

FecCodec::~FecCodec() = default;

std::vector<std::vector<char>> FecCodec::encode(std::span<const char> kcp_packet)
{
    if (!enabled_) {
        return {std::vector<char>(kcp_packet.begin(), kcp_packet.end())};
    }

    std::vector<std::vector<char>> packets;
    std::vector<char> data_packet(fec_header_size_plus_size + kcp_packet.size());
    write_le16(data_packet.data() + fec_header_size, static_cast<std::uint16_t>(kcp_packet.size() + 2));
    if (!kcp_packet.empty()) {
        std::memcpy(data_packet.data() + fec_header_size_plus_size, kcp_packet.data(), kcp_packet.size());
    }
    seal_packet(data_packet, fec_type_data);

    auto shard = std::vector<char>(data_packet.begin() + static_cast<std::ptrdiff_t>(fec_header_size), data_packet.end());
    max_shard_len_ = std::max(max_shard_len_, shard.size());
    encode_shards_[static_cast<std::size_t>(shard_count_)] = std::move(shard);
    ++shard_count_;

    packets.push_back(std::move(data_packet));

    const auto now = static_cast<std::int64_t>(current_millis());
    if (shard_count_ == data_shards_) {
        if (latest_packet_ms_ != 0 && now - latest_packet_ms_ < max_fec_encode_latency_ms) {
            for (auto& item : encode_shards_) {
                item.resize(max_shard_len_, 0);
            }
            std::vector<std::vector<char>> parity(static_cast<std::size_t>(parity_shards_),
                                                  std::vector<char>(max_shard_len_, 0));
            rs_->encode(encode_shards_, parity);
            for (auto& parity_shard : parity) {
                std::vector<char> parity_packet(fec_header_size + parity_shard.size());
                std::memcpy(parity_packet.data() + fec_header_size, parity_shard.data(), parity_shard.size());
                seal_packet(parity_packet, fec_type_parity);
                packets.push_back(std::move(parity_packet));
            }
        } else {
            skip_parity();
        }
        shard_count_ = 0;
        max_shard_len_ = 0;
    }

    latest_packet_ms_ = now;
    return packets;
}

std::vector<std::vector<char>> FecCodec::decode(std::span<const char> packet)
{
    if (!enabled_) {
        return {std::vector<char>(packet.begin(), packet.end())};
    }
    if (packet.size() < fec_header_size_plus_size) {
        return {std::vector<char>(packet.begin(), packet.end())};
    }

    const auto flag = packet_flag(packet);
    if (flag != fec_type_data && flag != fec_type_parity && flag != fec_type_oob) {
        return {std::vector<char>(packet.begin(), packet.end())};
    }
    if (flag == fec_type_oob) {
        snmp_add(SnmpField::oob_packets);
        return {};
    }

    std::vector<std::vector<char>> output;
    if (flag == fec_type_data) {
        output.emplace_back(packet.begin() + static_cast<std::ptrdiff_t>(fec_header_size_plus_size), packet.end());
    } else if (flag == fec_type_parity) {
        snmp_add(SnmpField::fec_parity_shards);
    }

    const auto seq = packet_seq(packet);
    if (seq >= paws_) {
        return output;
    }
    const auto shard_index = static_cast<int>(seq % static_cast<std::uint32_t>(shard_size_));
    const auto shard_id = seq / static_cast<std::uint32_t>(shard_size_);

    auto& set = decode_sets_[shard_id];
    if (set.shards.empty()) {
        set.shards.resize(static_cast<std::size_t>(shard_size_));
        set.present.resize(static_cast<std::size_t>(shard_size_), false);
        set.data_shard.resize(static_cast<std::size_t>(shard_size_), false);
        snmp_store(SnmpField::fec_shard_set, decode_sets_.size());
    }
    if (set.present[static_cast<std::size_t>(shard_index)]) {
        snmp_add(SnmpField::repeat_segs);
        return output;
    }

    set.shards[static_cast<std::size_t>(shard_index)] =
        std::vector<char>(packet.begin() + static_cast<std::ptrdiff_t>(fec_header_size), packet.end());
    set.present[static_cast<std::size_t>(shard_index)] = true;
    set.data_shard[static_cast<std::size_t>(shard_index)] = flag == fec_type_data;

    const auto present_count = static_cast<int>(std::ranges::count(set.present, true));
    if (present_count >= data_shards_) {
        auto data_present = 0;
        auto max_len = std::size_t {0};
        for (int i = 0; i < shard_size_; ++i) {
            if (!set.present[static_cast<std::size_t>(i)]) {
                continue;
            }
            if (i < data_shards_) {
                ++data_present;
            }
            max_len = std::max(max_len, set.shards[static_cast<std::size_t>(i)].size());
        }

        if (data_present != data_shards_) {
            for (int i = 0; i < shard_size_; ++i) {
                if (set.present[static_cast<std::size_t>(i)]) {
                    set.shards[static_cast<std::size_t>(i)].resize(max_len, 0);
                }
            }
            if (rs_->reconstruct_data(set.shards, set.present)) {
                std::uint64_t recovered_count = 0;
                for (int i = 0; i < data_shards_; ++i) {
                    if (set.present[static_cast<std::size_t>(i)]) {
                        continue;
                    }
                    const auto& recovered = set.shards[static_cast<std::size_t>(i)];
                    if (recovered.size() < 2) {
                        continue;
                    }
                    const auto size = read_le16(recovered.data());
                    if (size >= 2 && size <= recovered.size()) {
                        output.emplace_back(recovered.begin() + 2, recovered.begin() + size);
                        ++recovered_count;
                    }
                }
                snmp_add(SnmpField::fec_recovered, recovered_count);
            } else {
                snmp_add(SnmpField::fec_errs);
            }
        } else {
            snmp_add(SnmpField::fec_full_shard_set);
        }
        decode_sets_.erase(shard_id);
        snmp_store(SnmpField::fec_shard_set, decode_sets_.size());
    }

    if (timediff(shard_id * static_cast<std::uint32_t>(shard_size_),
                 newest_shard_id_ * static_cast<std::uint32_t>(shard_size_)) > 0) {
        newest_shard_id_ = shard_id;
        snmp_store(SnmpField::fec_shard_min, newest_shard_id_);
        discard_old_shards(newest_shard_id_);
    }

    return output;
}

std::uint16_t FecCodec::read_le16(const char* data)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::uint32_t FecCodec::read_le32(const char* data)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

void FecCodec::write_le16(char* data, std::uint16_t value)
{
    auto* bytes = reinterpret_cast<unsigned char*>(data);
    bytes[0] = static_cast<unsigned char>(value & 0xffU);
    bytes[1] = static_cast<unsigned char>((value >> 8U) & 0xffU);
}

void FecCodec::write_le32(char* data, std::uint32_t value)
{
    auto* bytes = reinterpret_cast<unsigned char*>(data);
    bytes[0] = static_cast<unsigned char>(value & 0xffU);
    bytes[1] = static_cast<unsigned char>((value >> 8U) & 0xffU);
    bytes[2] = static_cast<unsigned char>((value >> 16U) & 0xffU);
    bytes[3] = static_cast<unsigned char>((value >> 24U) & 0xffU);
}

std::uint16_t FecCodec::packet_flag(std::span<const char> packet)
{
    return read_le16(packet.data() + 4);
}

std::uint32_t FecCodec::packet_seq(std::span<const char> packet)
{
    return read_le32(packet.data());
}

void FecCodec::seal_packet(std::vector<char>& packet, std::uint16_t type)
{
    write_le32(packet.data(), next_seq_);
    write_le16(packet.data() + 4, type);
    next_seq_ = (next_seq_ + 1U) % paws_;
}

void FecCodec::skip_parity()
{
    next_seq_ = (next_seq_ + static_cast<std::uint32_t>(parity_shards_)) % paws_;
}

void FecCodec::discard_old_shards(std::uint32_t newest_shard_id)
{
    for (auto it = decode_sets_.begin(); it != decode_sets_.end();) {
        if (timediff(newest_shard_id * static_cast<std::uint32_t>(shard_size_),
                     it->first * static_cast<std::uint32_t>(shard_size_)) >
            static_cast<std::int32_t>(max_shard_sets * static_cast<std::uint32_t>(shard_size_))) {
            it = decode_sets_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace shimakaze
