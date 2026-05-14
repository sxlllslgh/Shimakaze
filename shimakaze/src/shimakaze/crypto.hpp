#pragma once

#include "shimakaze/config.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace CryptoPP {
class BlockTransformation;
}

namespace shimakaze {

class PacketCrypt final {
public:
    explicit PacketCrypt(const BaseConfig& config);
    ~PacketCrypt();

    std::vector<char> encrypt(std::span<const char> packet) const;
    std::optional<std::vector<char>> decrypt(std::span<const char> packet) const;

    std::size_t overhead() const noexcept;
    bool uses_header() const noexcept;
    bool encrypts_payload() const noexcept;
    std::string_view effective_method() const noexcept { return effective_method_; }

private:
    enum class Method {
        null,
        none,
        block,
        salsa20,
        xor_stream,
        aes_gcm,
    };

    static std::array<unsigned char, 32> derive_key(const std::string& key);
    static std::vector<unsigned char> derive_xor_table(std::span<const unsigned char> key);

    void block_cfb_crypt(std::span<unsigned char> data, bool encrypting) const;
    void salsa20_crypt(std::span<unsigned char> data) const;
    void xor_crypt(std::span<unsigned char> data) const;
    std::vector<char> aes_gcm_encrypt(std::span<const char> packet) const;
    std::optional<std::vector<char>> aes_gcm_decrypt(std::span<const char> packet) const;

    Method method_ = Method::null;
    std::string effective_method_ = "null";
    std::array<unsigned char, 32> derived_key_ {};
    std::array<unsigned char, 32> cipher_key_ {};
    std::vector<unsigned char> xor_table_;
    std::unique_ptr<CryptoPP::BlockTransformation> block_;
    std::size_t block_size_ = 0;
    std::size_t cipher_key_size_ = 0;
};

} // namespace shimakaze
