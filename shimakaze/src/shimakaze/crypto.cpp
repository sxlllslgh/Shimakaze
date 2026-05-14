#include "shimakaze/crypto.hpp"

#include <aes.h>
#include <blowfish.h>
#include <cast.h>
#include <des.h>
#include <gcm.h>
#include <modes.h>
#include <osrng.h>
#include <pwdbased.h>
#include <salsa.h>
#include <sha.h>
#include <sm4.h>
#include <tea.h>
#include <twofish.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace shimakaze {
namespace {

constexpr std::size_t nonce_size = 16;
constexpr std::size_t crc_size = 4;
constexpr std::size_t crypt_header_size = nonce_size + crc_size;
constexpr std::size_t mtu_limit = 1500;
constexpr std::size_t aes_gcm_nonce_size = 12;
constexpr std::size_t aes_gcm_tag_size = 16;
constexpr std::array<unsigned char, 16> initial_vector {
    167, 115, 79, 156, 18, 172, 27, 1, 164, 21, 242, 193, 252, 120, 230, 107,
};

std::string normalize_method(std::string_view method)
{
    std::string out;
    out.reserve(method.size());
    for (const char ch : method) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

std::uint32_t crc32_ieee(std::span<const char> data)
{
    static constexpr std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> values {};
        for (std::uint32_t i = 0; i < values.size(); ++i) {
            auto crc = i;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 1U) ? (0xedb88320U ^ (crc >> 1U)) : (crc >> 1U);
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

void fill_random(std::span<unsigned char> data)
{
    thread_local CryptoPP::AutoSeededRandomPool rng;
    rng.GenerateBlock(data.data(), data.size());
}

template <typename Cipher>
std::unique_ptr<CryptoPP::BlockTransformation> make_block_cipher(std::span<const unsigned char> key)
{
    auto cipher = std::make_unique<typename Cipher::Encryption>();
    cipher->SetKey(key.data(), key.size());
    return cipher;
}

std::unique_ptr<CryptoPP::BlockTransformation> make_tea_cipher(std::span<const unsigned char> key)
{
    auto cipher = std::make_unique<CryptoPP::TEA::Encryption>();
    cipher->SetKey(key.data(), key.size(), CryptoPP::MakeParameters(CryptoPP::Name::Rounds(), 16));
    return cipher;
}

std::span<const unsigned char> key_span(const std::array<unsigned char, 32>& key, std::size_t size)
{
    return std::span<const unsigned char>(key.data(), size == 0 ? key.size() : size);
}

} // namespace

PacketCrypt::PacketCrypt(const BaseConfig& config)
    : derived_key_(derive_key(config.key))
{
    const auto method = normalize_method(config.crypt);
    auto select_block = [this](std::string_view effective,
                               std::size_t key_size,
                               auto make_cipher) {
        effective_method_ = std::string(effective);
        method_ = Method::block;
        cipher_key_size_ = key_size == 0 ? derived_key_.size() : key_size;
        std::copy_n(derived_key_.begin(), cipher_key_size_, cipher_key_.begin());
        block_ = make_cipher(key_span(cipher_key_, cipher_key_size_));
        block_size_ = block_->BlockSize();
        if (block_size_ != 8 && block_size_ != 16) {
            throw std::runtime_error("unsupported Shimakaze block cipher size");
        }
    };

    try {
        if (method == "null") {
            method_ = Method::null;
            effective_method_ = "null";
        } else if (method == "none") {
            method_ = Method::none;
            effective_method_ = "none";
        } else if (method == "aes-128") {
            select_block("aes-128", 16, make_block_cipher<CryptoPP::AES>);
        } else if (method == "aes-192") {
            select_block("aes-192", 24, make_block_cipher<CryptoPP::AES>);
        } else if (method == "aes" || method == "aes-256") {
            select_block("aes", 32, make_block_cipher<CryptoPP::AES>);
        } else if (method == "blowfish") {
            select_block("blowfish", 0, make_block_cipher<CryptoPP::Blowfish>);
        } else if (method == "twofish") {
            select_block("twofish", 0, make_block_cipher<CryptoPP::Twofish>);
        } else if (method == "cast5") {
            select_block("cast5", 16, make_block_cipher<CryptoPP::CAST128>);
        } else if (method == "3des") {
            select_block("3des", 24, make_block_cipher<CryptoPP::DES_EDE3>);
        } else if (method == "tea") {
            select_block("tea", 16, make_tea_cipher);
        } else if (method == "xtea") {
            select_block("xtea", 16, make_block_cipher<CryptoPP::XTEA>);
        } else if (method == "sm4") {
            select_block("sm4", 16, make_block_cipher<CryptoPP::SM4>);
        } else if (method == "salsa20") {
            method_ = Method::salsa20;
            effective_method_ = "salsa20";
            cipher_key_size_ = derived_key_.size();
            std::copy_n(derived_key_.begin(), cipher_key_size_, cipher_key_.begin());
        } else if (method == "xor") {
            method_ = Method::xor_stream;
            effective_method_ = "xor";
            xor_table_ = derive_xor_table(std::span<const unsigned char>(derived_key_.data(), derived_key_.size()));
        } else if (method == "aes-128-gcm") {
            method_ = Method::aes_gcm;
            effective_method_ = "aes-128-gcm";
            cipher_key_size_ = 16;
            std::copy_n(derived_key_.begin(), cipher_key_size_, cipher_key_.begin());
        } else {
            select_block("aes", 32, make_block_cipher<CryptoPP::AES>);
        }
    } catch (const CryptoPP::Exception&) {
        select_block("aes", 32, make_block_cipher<CryptoPP::AES>);
    }
}

PacketCrypt::~PacketCrypt() = default;

std::vector<char> PacketCrypt::encrypt(std::span<const char> packet) const
{
    if (method_ == Method::null) {
        return {packet.begin(), packet.end()};
    }
    if (method_ == Method::aes_gcm) {
        return aes_gcm_encrypt(packet);
    }

    std::vector<char> output(crypt_header_size + packet.size());
    fill_random(std::span<unsigned char>(reinterpret_cast<unsigned char*>(output.data()), nonce_size));
    write_le32(output.data() + nonce_size, crc32_ieee(packet));
    if (!packet.empty()) {
        std::memcpy(output.data() + crypt_header_size, packet.data(), packet.size());
    }

    auto bytes = std::span<unsigned char>(reinterpret_cast<unsigned char*>(output.data()), output.size());
    switch (method_) {
    case Method::none:
        break;
    case Method::block:
        block_cfb_crypt(bytes, true);
        break;
    case Method::salsa20:
        salsa20_crypt(bytes);
        break;
    case Method::xor_stream:
        xor_crypt(bytes);
        break;
    case Method::null:
    case Method::aes_gcm:
        break;
    }
    return output;
}

std::optional<std::vector<char>> PacketCrypt::decrypt(std::span<const char> packet) const
{
    if (method_ == Method::null) {
        return std::vector<char>(packet.begin(), packet.end());
    }
    if (method_ == Method::aes_gcm) {
        return aes_gcm_decrypt(packet);
    }
    if (packet.size() < crypt_header_size) {
        return std::nullopt;
    }

    std::vector<char> decoded(packet.begin(), packet.end());
    auto bytes = std::span<unsigned char>(reinterpret_cast<unsigned char*>(decoded.data()), decoded.size());
    switch (method_) {
    case Method::none:
        break;
    case Method::block:
        block_cfb_crypt(bytes, false);
        break;
    case Method::salsa20:
        salsa20_crypt(bytes);
        break;
    case Method::xor_stream:
        xor_crypt(bytes);
        break;
    case Method::null:
    case Method::aes_gcm:
        break;
    }

    const auto payload = std::span<const char>(decoded.data() + crypt_header_size, decoded.size() - crypt_header_size);
    if (crc32_ieee(payload) != read_le32(decoded.data() + nonce_size)) {
        return std::nullopt;
    }
    return std::vector<char>(payload.begin(), payload.end());
}

std::size_t PacketCrypt::overhead() const noexcept
{
    if (method_ == Method::null) {
        return 0;
    }
    if (method_ == Method::aes_gcm) {
        return aes_gcm_nonce_size + aes_gcm_tag_size;
    }
    return crypt_header_size;
}

bool PacketCrypt::uses_header() const noexcept
{
    return method_ != Method::null;
}

bool PacketCrypt::encrypts_payload() const noexcept
{
    return method_ != Method::null && method_ != Method::none;
}

std::array<unsigned char, 32> PacketCrypt::derive_key(const std::string& key)
{
    std::array<unsigned char, 32> out {};
    static constexpr std::string_view salt = "kcp-go";
    CryptoPP::PKCS5_PBKDF2_HMAC<CryptoPP::SHA1> pbkdf;
    pbkdf.DeriveKey(out.data(),
                    out.size(),
                    0,
                    reinterpret_cast<const CryptoPP::byte*>(key.data()),
                    key.size(),
                    reinterpret_cast<const CryptoPP::byte*>(salt.data()),
                    salt.size(),
                    4096);
    return out;
}

std::vector<unsigned char> PacketCrypt::derive_xor_table(std::span<const unsigned char> key)
{
    std::vector<unsigned char> out(mtu_limit);
    static constexpr std::string_view salt = "sH3CIVoF#rWLtJo6";
    CryptoPP::PKCS5_PBKDF2_HMAC<CryptoPP::SHA1> pbkdf;
    pbkdf.DeriveKey(out.data(),
                    out.size(),
                    0,
                    key.data(),
                    key.size(),
                    reinterpret_cast<const CryptoPP::byte*>(salt.data()),
                    salt.size(),
                    32);
    return out;
}

void PacketCrypt::block_cfb_crypt(std::span<unsigned char> data, bool encrypting) const
{
    if (!block_) {
        return;
    }

    std::array<unsigned char, 16> current {};
    std::array<unsigned char, 16> next {};
    block_->ProcessBlock(initial_vector.data(), current.data());

    for (std::size_t offset = 0; offset < data.size();) {
        const auto remaining = data.size() - offset;
        const auto count = std::min(block_size_, remaining);
        if (!encrypting && count == block_size_) {
            block_->ProcessBlock(data.data() + offset, next.data());
        }
        for (std::size_t i = 0; i < count; ++i) {
            data[offset + i] ^= current[i];
        }
        if (count != block_size_) {
            break;
        }
        if (encrypting) {
            block_->ProcessBlock(data.data() + offset, current.data());
        } else {
            std::copy_n(next.begin(), block_size_, current.begin());
        }
        offset += block_size_;
    }
}

void PacketCrypt::salsa20_crypt(std::span<unsigned char> data) const
{
    if (data.size() <= 8) {
        return;
    }

    CryptoPP::Salsa20::Encryption cipher;
    cipher.SetKeyWithIV(cipher_key_.data(), 32, data.data(), 8);
    cipher.ProcessData(data.data() + 8, data.data() + 8, data.size() - 8);
}

void PacketCrypt::xor_crypt(std::span<unsigned char> data) const
{
    if (xor_table_.empty()) {
        return;
    }

    const auto count = std::min(data.size(), xor_table_.size());
    for (std::size_t i = 0; i < count; ++i) {
        data[i] ^= xor_table_[i];
    }
}

std::vector<char> PacketCrypt::aes_gcm_encrypt(std::span<const char> packet) const
{
    if (packet.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("packet too large for AES-GCM encryption");
    }

    std::vector<char> output(aes_gcm_nonce_size + packet.size() + aes_gcm_tag_size);
    auto nonce = std::span<unsigned char>(reinterpret_cast<unsigned char*>(output.data()), aes_gcm_nonce_size);
    fill_random(nonce);

    CryptoPP::GCM<CryptoPP::AES>::Encryption enc;
    enc.SetKeyWithIV(cipher_key_.data(), cipher_key_size_, nonce.data(), nonce.size());
    enc.SpecifyDataLengths(0, packet.size(), 0);
    enc.ProcessData(reinterpret_cast<CryptoPP::byte*>(output.data() + aes_gcm_nonce_size),
                    reinterpret_cast<const CryptoPP::byte*>(packet.data()),
                    packet.size());
    enc.TruncatedFinal(reinterpret_cast<CryptoPP::byte*>(output.data() + aes_gcm_nonce_size + packet.size()),
                       aes_gcm_tag_size);
    return output;
}

std::optional<std::vector<char>> PacketCrypt::aes_gcm_decrypt(std::span<const char> packet) const
{
    if (packet.size() < aes_gcm_nonce_size + aes_gcm_tag_size) {
        return std::nullopt;
    }

    const auto payload_size = packet.size() - aes_gcm_nonce_size - aes_gcm_tag_size;
    std::vector<char> output(payload_size);
    try {
        CryptoPP::GCM<CryptoPP::AES>::Decryption dec;
        dec.SetKeyWithIV(cipher_key_.data(),
                         cipher_key_size_,
                         reinterpret_cast<const CryptoPP::byte*>(packet.data()),
                         aes_gcm_nonce_size);
        dec.SpecifyDataLengths(0, payload_size, 0);
        dec.ProcessData(reinterpret_cast<CryptoPP::byte*>(output.data()),
                        reinterpret_cast<const CryptoPP::byte*>(packet.data() + aes_gcm_nonce_size),
                        payload_size);
        const auto ok = dec.TruncatedVerify(
            reinterpret_cast<const CryptoPP::byte*>(packet.data() + aes_gcm_nonce_size + payload_size),
            aes_gcm_tag_size);
        if (!ok) {
            return std::nullopt;
        }
    } catch (const CryptoPP::Exception&) {
        return std::nullopt;
    }
    return output;
}

} // namespace shimakaze
