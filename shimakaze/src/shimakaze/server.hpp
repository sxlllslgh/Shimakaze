#pragma once

#include "shimakaze/config.hpp"
#include "shimakaze/crypto.hpp"
#include "shimakaze/smux.hpp"
#include "shimakaze/utils.hpp"

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace shimakaze {

class Server final {
public:
    Server(asio::io_context& io, ServerConfig config);

    void start();

private:
    class UdpListener final {
    public:
        UdpListener(Server& server, std::uint16_t port);
        void start();

    private:
        struct SessionEntry {
            std::shared_ptr<KcpTransport> transport;
            std::shared_ptr<SmuxSession> smux;
        };

        void start_receive();
        void handle_packet(std::size_t bytes);

        Server& server_;
        std::uint16_t port_ = 0;
        udp::socket socket_;
        udp::endpoint sender_;
        std::array<char, 64 * 1024> buffer_ {};
        std::unordered_map<std::string, SessionEntry> sessions_;
    };

    asio::io_context& io_;
    ServerConfig config_;
    PacketCrypt crypt_;
    MultiPort listen_address_;
    MultiPort target_address_;
    std::string target_path_;
    std::vector<std::unique_ptr<UdpListener>> listeners_;
    bool target_is_unix_ = false;
};

} // namespace shimakaze
