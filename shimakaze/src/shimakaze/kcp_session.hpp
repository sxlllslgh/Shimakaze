#pragma once

#include "shimakaze/config.hpp"
#include "shimakaze/crypto.hpp"
#include "shimakaze/utils.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct IKCPCB;
using ikcpcb = IKCPCB;

namespace shimakaze {

class KcpSession final : public std::enable_shared_from_this<KcpSession> {
public:
    using CloseHandler = std::function<void(std::uint32_t)>;

    KcpSession(asio::io_context& io,
               udp::socket& udp_socket,
               udp::endpoint remote_endpoint,
               std::uint32_t conv,
               BaseConfig config,
               CloseHandler close_handler);
    ~KcpSession();

    KcpSession(const KcpSession&) = delete;
    KcpSession& operator=(const KcpSession&) = delete;

    std::uint32_t conv() const noexcept { return conv_; }
    const udp::endpoint& remote_endpoint() const noexcept { return remote_endpoint_; }

    void start_client(tcp::socket tcp_socket);
    void start_server(std::string target_host, std::uint16_t target_port);
    void input(std::span<const char> packet);
    void close();

private:
    enum class FrameType : std::uint8_t {
        data = 1,
        fin = 2,
    };

    static int output_callback(const char* buffer, int length, ikcpcb* kcp, void* user);

    void configure_kcp();
    void start_update_timer();
    void update_kcp();
    void send_udp(const char* buffer, int length);

    void start_tcp_read();
    void send_frame(FrameType type, std::span<const char> payload = {});
    void drain_kcp();
    void parse_frames();
    void handle_data_frame(std::span<const char> payload);
    void handle_fin_frame();

    void enqueue_tcp_write(std::vector<char> data);
    void flush_tcp_writes();
    void send_local_fin();
    void maybe_finish();
    void schedule_close_after_fin();
    void fail_and_close(std::string_view where, const boost::system::error_code& error);

    asio::io_context& io_;
    udp::socket& udp_socket_;
    udp::endpoint remote_endpoint_;
    tcp::socket tcp_socket_;
    tcp::resolver resolver_;
    asio::steady_timer update_timer_;
    asio::steady_timer close_timer_;
    std::uint32_t conv_ = 0;
    BaseConfig config_;
    PacketCrypt crypt_;
    CloseHandler close_handler_;
    ikcpcb* kcp_ = nullptr;

    std::array<char, 32 * 1024> tcp_read_buffer_ {};
    std::vector<char> kcp_stream_buffer_;
    std::deque<std::vector<char>> pending_tcp_writes_;
    bool tcp_write_active_ = false;
    bool tcp_ready_ = false;
    bool closing_ = false;
    bool local_fin_sent_ = false;
    bool remote_fin_received_ = false;
    bool server_connecting_ = false;
};

} // namespace shimakaze
