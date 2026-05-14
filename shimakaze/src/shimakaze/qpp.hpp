#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace shimakaze {

class QppPad final {
public:
    static constexpr std::uint8_t qubits = 8;
    static constexpr std::uint16_t minimum_pads = 7;
    static constexpr std::size_t minimum_seed_length = 211;

    explicit QppPad(std::string_view seed, std::uint16_t num_pads);

    std::uint16_t num_pads() const noexcept { return num_pads_; }

    class Rand final {
    public:
        std::array<std::uint64_t, 4> xoshiro {};
        std::uint64_t seed64 = 0;
        std::uint8_t count = 0;
    };

    static Rand create_prng(std::string_view seed);

    void encrypt(std::span<char> data, Rand& rand) const;
    void decrypt(std::span<char> data, Rand& rand) const;

private:
    static constexpr std::size_t matrix_bytes = 1U << qubits;

    static std::vector<std::array<std::uint8_t, 32>> seed_to_chunks(std::string_view seed);
    static void fill_pad(std::span<std::uint8_t, matrix_bytes> pad);
    static void reverse_pad(std::span<const std::uint8_t, matrix_bytes> pad,
                            std::span<std::uint8_t, matrix_bytes> rpad);
    void shuffle_pad(std::span<const std::uint8_t, 32> chunk,
                     std::span<std::uint8_t, matrix_bytes> pad,
                     std::uint16_t pad_id,
                     const std::vector<std::array<std::uint8_t, 32>>& aes_keys);

    std::vector<std::uint8_t> pads_;
    std::vector<std::uint8_t> rpads_;
    std::uint16_t num_pads_ = 0;
};

class QppStream final {
public:
    QppStream() = default;
    QppStream(std::shared_ptr<const QppPad> pad, std::string_view seed);

    bool enabled() const noexcept { return pad_ != nullptr; }

    void encrypt(std::span<char> data);
    void decrypt(std::span<char> data);

private:
    std::shared_ptr<const QppPad> pad_;
    QppPad::Rand write_rand_;
    QppPad::Rand read_rand_;
};

std::vector<std::string> validate_qpp_params(int count, std::string_view key);

} // namespace shimakaze
