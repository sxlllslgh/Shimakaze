#include "shimakaze/config.hpp"
#include "shimakaze/crypto.hpp"
#include "shimakaze/fec.hpp"
#include "shimakaze/qpp.hpp"
#include "shimakaze/snappy_stream.hpp"
#include "shimakaze/utils.hpp"

#include <cassert>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <iterator>
#include <ranges>
#include <string>
#include <vector>

int main()
{
    {
        auto ports = shimakaze::parse_multiport("127.0.0.1:29900-29999");
        assert(ports.host == "127.0.0.1");
        assert(ports.min_port == 29900);
        assert(ports.max_port == 29999);
    }

    {
        auto ports = shimakaze::parse_multiport(":12948");
        assert(ports.host.empty());
        assert(ports.min_port == 12948);
        assert(ports.max_port == 12948);
    }

    {
        shimakaze::ClientConfig config;
        config.mode = "fast3";
        shimakaze::apply_mode(config);
        assert(config.nodelay == 1);
        assert(config.interval == 10);
        assert(config.resend == 2);
        assert(config.nc == 1);
    }

    {
        char b[4] {};
        shimakaze::write_be32(b, 0x01020304U);
        assert(static_cast<unsigned char>(b[0]) == 0x01);
        assert(static_cast<unsigned char>(b[1]) == 0x02);
        assert(static_cast<unsigned char>(b[2]) == 0x03);
        assert(static_cast<unsigned char>(b[3]) == 0x04);
        assert(shimakaze::read_be32(b) == 0x01020304U);
    }

    {
        const std::vector<std::string> methods {
            "null", "none", "aes", "aes-128", "aes-192", "aes-256",
            "blowfish", "twofish", "cast5", "3des", "tea", "xtea",
            "sm4", "salsa20", "xor", "aes-128-gcm",
        };
        const std::string payload = "shimakaze crypto roundtrip";
        for (const auto& method : methods) {
            shimakaze::BaseConfig config;
            config.key = "test secret";
            config.crypt = method;
            shimakaze::PacketCrypt crypt(config);
            auto encrypted = crypt.encrypt(std::span<const char>(payload.data(), payload.size()));
            auto decrypted = crypt.decrypt(std::span<const char>(encrypted.data(), encrypted.size()));
            assert(decrypted);
            assert(std::string(decrypted->begin(), decrypted->end()) == payload);
        }
    }

    {
        shimakaze::BaseConfig config;
        config.datashard = 3;
        config.parityshard = 1;
        shimakaze::FecCodec encoder(config);
        shimakaze::FecCodec decoder(config);

        const std::vector<std::string> packets {"packet-one", "packet-two", "packet-three"};
        std::vector<std::vector<char>> encoded;
        for (const auto& packet : packets) {
            auto out = encoder.encode(std::span<const char>(packet.data(), packet.size()));
            encoded.insert(encoded.end(), std::make_move_iterator(out.begin()), std::make_move_iterator(out.end()));
        }

        std::vector<std::string> decoded;
        for (std::size_t i = 0; i < encoded.size(); ++i) {
            if (i == 1) {
                continue;
            }
            auto out = decoder.decode(std::span<const char>(encoded[i].data(), encoded[i].size()));
            for (const auto& item : out) {
                decoded.emplace_back(item.begin(), item.end());
            }
        }
        for (const auto& packet : packets) {
            assert(std::ranges::find(decoded, packet) != decoded.end());
        }
    }

    {
        shimakaze::SnappyStreamEncoder encoder;
        shimakaze::SnappyStreamDecoder decoder;
        const std::string payload(70000, 'x');
        auto chunks = encoder.encode(std::span<const char>(payload.data(), payload.size()));
        std::string decoded;
        for (const auto& chunk : chunks) {
            auto out = decoder.decode(std::span<const char>(chunk.data(), chunk.size()));
            for (const auto& item : out) {
                decoded.append(item.begin(), item.end());
            }
        }
        assert(decoded == payload);
    }

    {
        const std::string seed = "qpp-test-seed-with-enough-material";
        auto pad = std::make_shared<shimakaze::QppPad>(seed, 61);
        shimakaze::QppStream writer(pad, seed);
        shimakaze::QppStream reader(pad, seed);
        std::string payload = "QPP split-stream roundtrip payload";
        std::vector<char> first(payload.begin(), payload.begin() + 7);
        std::vector<char> second(payload.begin() + 7, payload.end());
        writer.encrypt(std::span<char>(first.data(), first.size()));
        writer.encrypt(std::span<char>(second.data(), second.size()));
        reader.decrypt(std::span<char>(first.data(), first.size()));
        reader.decrypt(std::span<char>(second.data(), second.size()));
        first.insert(first.end(), second.begin(), second.end());
        assert(std::string(first.begin(), first.end()) == payload);
    }

    std::cout << "config tests passed\n";
    return 0;
}
