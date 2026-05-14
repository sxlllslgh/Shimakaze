#pragma once

#include "shimakaze/config.hpp"
#include "shimakaze/crypto.hpp"
#include "shimakaze/fec.hpp"
#include "shimakaze/utils.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <vector>

struct IKCPCB;
using ikcpcb = IKCPCB;

namespace shimakaze {

class KcpTransport final : public std::enable_shared_from_this<KcpTransport> {
public:
    using ReadHandler = std::function<void(std::span<const char>)>;
    using CloseHandler = std::function<void()>;

    KcpTransport(asio::io_context& io,
                 udp::socket& udp_socket,
                 udp::endpoint remote_endpoint,
                 std::uint32_t conv,
                 BaseConfig config,
                 CloseHandler close_handler);
    ~KcpTransport();

    KcpTransport(const KcpTransport&) = delete;
    KcpTransport& operator=(const KcpTransport&) = delete;

    std::uint32_t conv() const noexcept { return conv_; }
    const udp::endpoint& remote_endpoint() const noexcept { return remote_endpoint_; }
    bool is_closed() const noexcept { return closing_; }

    void set_read_handler(ReadHandler handler);
    void start();
    void input(std::span<const char> packet);
    void send(std::span<const char> bytes);
    void close();

private:
    static int output_callback(const char* buffer, int length, ikcpcb* kcp, void* user);

    void configure_kcp();
    void start_update_timer();
    void update_kcp();
    void drain_kcp();
    void send_udp(const char* buffer, int length);
    void enqueue_udp_packet(std::shared_ptr<std::vector<char>> payload);
    void schedule_rate_limited_send();

    asio::io_context& io_;
    udp::socket& udp_socket_;
    udp::endpoint remote_endpoint_;
    asio::steady_timer update_timer_;
    asio::steady_timer rate_timer_;
    std::uint32_t conv_ = 0;
    BaseConfig config_;
    PacketCrypt crypt_;
    FecCodec fec_;
    CloseHandler close_handler_;
    ReadHandler read_handler_;
    ikcpcb* kcp_ = nullptr;
    std::deque<std::shared_ptr<std::vector<char>>> pending_udp_;
    std::chrono::steady_clock::time_point next_tx_time_ {};
    bool rate_send_active_ = false;
    bool closing_ = false;
};

} // namespace shimakaze
