#include "shimakaze/qpp.hpp"

#include <aes.h>
#include <hmac.h>
#include <pwdbased.h>
#include <sha.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <numeric>
#include <stdexcept>

namespace shimakaze {
namespace {

constexpr std::string_view pad_identifier_prefix = "QPP_";
constexpr std::string_view pm_selector_identifier = "PERMUTATION_MATRIX_SELECTOR";
constexpr std::string_view shuffle_salt = "___QUANTUM_PERMUTATION_PAD_SHUFFLE_SALT___";
constexpr std::string_view prng_salt = "___QUANTUM_PERMUTATION_PAD_PRNG_SALT___";
constexpr std::string_view chunk_derive_salt = "___QUANTUM_PERMUTATION_PAD_SEED_DERIVE___";
constexpr int pbkdf2_loops = 128;
constexpr int chunk_derive_loops = 1024;

std::array<std::uint8_t, 32> pbkdf2_sha1(std::span<const std::uint8_t> seed,
                                         std::string_view salt,
                                         int iterations)
{
    std::array<std::uint8_t, 32> out {};
    CryptoPP::PKCS5_PBKDF2_HMAC<CryptoPP::SHA1> pbkdf;
    pbkdf.DeriveKey(out.data(),
                    out.size(),
                    0,
                    seed.data(),
                    seed.size(),
                    reinterpret_cast<const CryptoPP::byte*>(salt.data()),
                    salt.size(),
                    iterations);
    return out;
}

std::array<std::uint8_t, 32> pbkdf2_sha1(std::string_view seed, std::string_view salt, int iterations)
{
    return pbkdf2_sha1(std::span<const std::uint8_t>(
                           reinterpret_cast<const std::uint8_t*>(seed.data()), seed.size()),
                       salt,
                       iterations);
}

std::array<std::uint8_t, 32> hmac_sha256(std::span<const std::uint8_t> key, std::string_view message)
{
    std::array<std::uint8_t, 32> out {};
    CryptoPP::HMAC<CryptoPP::SHA256> hmac(key.data(), key.size());
    hmac.Update(reinterpret_cast<const CryptoPP::byte*>(message.data()), message.size());
    hmac.Final(out.data());
    return out;
}

std::array<std::uint8_t, 32> hmac_sha256(std::string_view key, std::string_view message)
{
    return hmac_sha256(std::span<const std::uint8_t>(
                           reinterpret_cast<const std::uint8_t*>(key.data()), key.size()),
                       message);
}

std::uint64_t read_le64(std::span<const std::uint8_t, 8> bytes)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8U);
    }
    return value;
}

std::uint64_t xoshiro256ss(std::array<std::uint64_t, 4>& state)
{
    const auto result = std::rotl(state[1] * 5U, 7) * 9U;
    const auto t = state[1] << 17U;

    state[2] ^= state[0];
    state[3] ^= state[1];
    state[1] ^= state[2];
    state[0] ^= state[3];
    state[2] ^= t;
    state[3] = std::rotl(state[3], 45);
    return result;
}

std::string binary_string(std::uint16_t value)
{
    if (value == 0) {
        return "0";
    }

    std::string out;
    while (value != 0) {
        out.push_back((value & 1U) != 0 ? '1' : '0');
        value >>= 1U;
    }
    std::ranges::reverse(out);
    return out;
}

std::uint32_t mod_big_endian(std::span<const std::uint8_t> bytes, std::uint32_t modulus)
{
    std::uint32_t result = 0;
    for (const auto byte : bytes) {
        result = static_cast<std::uint32_t>((static_cast<std::uint64_t>(result) * 256U + byte) % modulus);
    }
    return result;
}

bool coprime(int a, int b)
{
    return std::gcd(a, b) == 1;
}

} // namespace

QppPad::QppPad(std::string_view seed, std::uint16_t num_pads)
    : pads_(static_cast<std::size_t>(num_pads) * matrix_bytes)
    , rpads_(static_cast<std::size_t>(num_pads) * matrix_bytes)
    , num_pads_(num_pads)
{
    if (num_pads_ == 0) {
        throw std::runtime_error("QPPCount must be greater than 0 when QPP is enabled");
    }

    const auto chunks = seed_to_chunks(seed);
    std::vector<std::array<std::uint8_t, 32>> aes_keys;
    aes_keys.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        aes_keys.push_back(pbkdf2_sha1(std::span<const std::uint8_t>(chunk.data(), chunk.size()),
                                       shuffle_salt,
                                       pbkdf2_loops));
    }

    for (std::uint16_t i = 0; i < num_pads_; ++i) {
        auto pad = std::span<std::uint8_t, matrix_bytes>(pads_.data() + static_cast<std::size_t>(i) * matrix_bytes,
                                                         matrix_bytes);
        auto rpad = std::span<std::uint8_t, matrix_bytes>(rpads_.data() + static_cast<std::size_t>(i) * matrix_bytes,
                                                          matrix_bytes);
        fill_pad(pad);
        shuffle_pad(chunks[static_cast<std::size_t>(i) % chunks.size()], pad, i, aes_keys);
        reverse_pad(pad, rpad);
    }
}

QppPad::Rand QppPad::create_prng(std::string_view seed)
{
    auto mac = hmac_sha256(seed, pm_selector_identifier);
    auto xoshiro = pbkdf2_sha1(std::span<const std::uint8_t>(mac.data(), mac.size()), prng_salt, pbkdf2_loops);

    Rand rand;
    rand.xoshiro[0] = read_le64(std::span<const std::uint8_t, 8>(xoshiro.data(), 8));
    rand.xoshiro[1] = read_le64(std::span<const std::uint8_t, 8>(xoshiro.data() + 8, 8));
    rand.xoshiro[2] = read_le64(std::span<const std::uint8_t, 8>(xoshiro.data() + 16, 8));
    rand.xoshiro[3] = read_le64(std::span<const std::uint8_t, 8>(xoshiro.data() + 24, 8));
    rand.seed64 = xoshiro256ss(rand.xoshiro);
    return rand;
}

void QppPad::encrypt(std::span<char> data, Rand& rand) const
{
    auto r = rand.seed64;
    auto count = rand.count;
    for (auto& item : data) {
        const auto pad_index = static_cast<std::size_t>(static_cast<std::uint16_t>(r) % num_pads_) * matrix_bytes;
        const auto rr = static_cast<std::uint8_t>(r >> (count * 8U));
        const auto plain = static_cast<std::uint8_t>(item);
        item = static_cast<char>(pads_[pad_index + static_cast<std::size_t>(plain ^ rr)]);
        ++count;
        if (count == 8) {
            r = xoshiro256ss(rand.xoshiro);
            count = 0;
        }
    }
    rand.seed64 = r;
    rand.count = count;
}

void QppPad::decrypt(std::span<char> data, Rand& rand) const
{
    auto r = rand.seed64;
    auto count = rand.count;
    for (auto& item : data) {
        const auto pad_index = static_cast<std::size_t>(static_cast<std::uint16_t>(r) % num_pads_) * matrix_bytes;
        const auto rr = static_cast<std::uint8_t>(r >> (count * 8U));
        const auto cipher = static_cast<std::uint8_t>(item);
        item = static_cast<char>(rpads_[pad_index + static_cast<std::size_t>(cipher)] ^ rr);
        ++count;
        if (count == 8) {
            r = xoshiro256ss(rand.xoshiro);
            count = 0;
        }
    }
    rand.seed64 = r;
    rand.count = count;
}

std::vector<std::array<std::uint8_t, 32>> QppPad::seed_to_chunks(std::string_view seed)
{
    std::vector<std::uint8_t> expanded(reinterpret_cast<const std::uint8_t*>(seed.data()),
                                       reinterpret_cast<const std::uint8_t*>(seed.data()) + seed.size());
    if (expanded.size() < 32) {
        auto derived = pbkdf2_sha1(seed, chunk_derive_salt, pbkdf2_loops);
        expanded.assign(derived.begin(), derived.end());
    }

    const auto chunk_count = (minimum_seed_length + 31U) / 32U;
    std::vector<std::array<std::uint8_t, 32>> chunks(chunk_count);
    std::size_t seed_index = 0;
    for (auto& chunk : chunks) {
        for (auto& byte : chunk) {
            byte = expanded[seed_index % expanded.size()];
            ++seed_index;
        }
        const auto derived = pbkdf2_sha1(std::span<const std::uint8_t>(chunk.data(), chunk.size()),
                                         chunk_derive_salt,
                                         chunk_derive_loops);
        chunk = derived;
    }
    return chunks;
}

void QppPad::fill_pad(std::span<std::uint8_t, matrix_bytes> pad)
{
    for (std::size_t i = 0; i < pad.size(); ++i) {
        pad[i] = static_cast<std::uint8_t>(i);
    }
}

void QppPad::reverse_pad(std::span<const std::uint8_t, matrix_bytes> pad,
                         std::span<std::uint8_t, matrix_bytes> rpad)
{
    for (std::size_t i = 0; i < pad.size(); ++i) {
        rpad[pad[i]] = static_cast<std::uint8_t>(i);
    }
}

void QppPad::shuffle_pad(std::span<const std::uint8_t, 32> chunk,
                         std::span<std::uint8_t, matrix_bytes> pad,
                         std::uint16_t pad_id,
                         const std::vector<std::array<std::uint8_t, 32>>& aes_keys)
{
    auto message = std::string(pad_identifier_prefix);
    message += binary_string(pad_id);
    auto sum = hmac_sha256(chunk, message);

    std::vector<CryptoPP::AES::Encryption> aes_blocks;
    aes_blocks.reserve(aes_keys.size());
    for (const auto& key : aes_keys) {
        aes_blocks.emplace_back(key.data(), key.size());
    }

    for (std::size_t i = pad.size() - 1; i > 0; --i) {
        for (auto& block : aes_blocks) {
            for (std::size_t offset = 0; offset < sum.size(); offset += CryptoPP::AES::BLOCKSIZE) {
                block.ProcessBlock(sum.data() + offset, sum.data() + offset);
            }
        }
        const auto j = mod_big_endian(sum, static_cast<std::uint32_t>(i + 1));
        std::swap(pad[i], pad[j]);
    }
}

QppStream::QppStream(std::shared_ptr<const QppPad> pad, std::string_view seed)
    : pad_(std::move(pad))
{
    if (pad_) {
        write_rand_ = QppPad::create_prng(seed);
        read_rand_ = QppPad::create_prng(seed);
    }
}

void QppStream::encrypt(std::span<char> data)
{
    if (pad_) {
        pad_->encrypt(data, write_rand_);
    }
}

void QppStream::decrypt(std::span<char> data)
{
    if (pad_) {
        pad_->decrypt(data, read_rand_);
    }
}

std::vector<std::string> validate_qpp_params(int count, std::string_view key)
{
    if (count <= 0) {
        throw std::runtime_error("QPPCount must be greater than 0 when QPP is enabled");
    }

    std::vector<std::string> warnings;
    if (key.size() < QppPad::minimum_seed_length) {
        warnings.push_back("QPP Warning: 'key' has size of " + std::to_string(key.size()) +
                           " bytes, required " + std::to_string(QppPad::minimum_seed_length) +
                           " bytes at least");
    }
    if (count < QppPad::minimum_pads) {
        warnings.push_back("QPP Warning: QPPCount " + std::to_string(count) +
                           ", required " + std::to_string(QppPad::minimum_pads) + " at least");
    }
    if (!coprime(count, QppPad::qubits)) {
        warnings.push_back("QPP Warning: QPPCount " + std::to_string(count) +
                           ", choose a prime number for security");
    }
    return warnings;
}

} // namespace shimakaze
