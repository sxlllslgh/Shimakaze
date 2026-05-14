#pragma once

#include "shimakaze/config.hpp"
#include "shimakaze/crypto.hpp"
#include "shimakaze/smux.hpp"
#include "shimakaze/utils.hpp"

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace shimakaze {

class Client final {
public:
    Client(asio::io_context& io, ClientConfig config);

    void start();

private:
    struct Connection {
        explicit Connection(asio::io_context& io)
            : socket(io)
        {
        }

        udp::socket socket;
        udp::endpoint sender;
        std::array<char, 64 * 1024> buffer {};
        std::shared_ptr<KcpTransport> transport;
        std::shared_ptr<SmuxSession> smux;
        std::chrono::steady_clock::time_point expiry {};
        std::uint32_t conv = 0;
    };

    struct TimedSession {
        std::shared_ptr<Connection> connection;
    };

    void start_accept();
    void handle_accept(tcp::socket socket);
#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
    void start_local_accept();
    void handle_accept(boost::asio::local::stream_protocol::socket socket);
#endif
    void start_udp_receive(const std::shared_ptr<Connection>& connection);
    std::shared_ptr<SmuxSession> ensure_session(std::size_t index);
    std::shared_ptr<Connection> create_connection();
    void schedule_scavenge(std::shared_ptr<Connection> connection, std::chrono::seconds delay);
    udp::endpoint pick_remote_endpoint();
    std::uint32_t next_conv() const;

    asio::io_context& io_;
    ClientConfig config_;
    PacketCrypt crypt_;
    MultiPort local_address_;
    MultiPort remote_address_;
    std::string local_path_;
    tcp::acceptor acceptor_;
#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
    std::unique_ptr<boost::asio::local::stream_protocol::acceptor> local_acceptor_;
#endif
    std::unordered_map<std::uint32_t, std::shared_ptr<KcpTransport>> transports_;
    std::vector<TimedSession> muxes_;
    std::vector<std::shared_ptr<asio::steady_timer>> scavenge_timers_;
    std::size_t rr_ = 0;
    bool local_is_unix_ = false;
};

} // namespace shimakaze
