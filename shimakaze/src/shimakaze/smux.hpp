#pragma once

#include "shimakaze/config.hpp"
#include "shimakaze/kcp_transport.hpp"
#include "shimakaze/qpp.hpp"
#include "shimakaze/snappy_stream.hpp"
#include "shimakaze/utils.hpp"

#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
#include <boost/asio/local/stream_protocol.hpp>
#endif

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace shimakaze {

class SmuxSession;

class SmuxStream final : public std::enable_shared_from_this<SmuxStream> {
public:
    SmuxStream(asio::io_context& io,
               std::weak_ptr<SmuxSession> session,
               std::uint32_t id,
               BaseConfig config,
               std::shared_ptr<const QppPad> qpp_pad);

    std::uint32_t id() const noexcept { return id_; }
    bool is_closed() const noexcept { return closed_; }

    void start_client(tcp::socket tcp_socket);
#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
    void start_client(boost::asio::local::stream_protocol::socket local_socket);
#endif
    void start_server(std::string target_host, std::uint16_t target_port);
#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
    void start_server_unix(std::string target_path);
#endif
    void on_data(std::span<const char> payload);
    void on_fin();
    void on_update(std::uint32_t consumed, std::uint32_t window);
    void close();

private:
    struct PendingBuffer {
        std::vector<char> data;
        std::size_t offset = 0;
    };

    void start_tcp_read();
    void queue_outbound(std::span<const char> payload);
    void try_flush_outbound();
    std::int64_t available_peer_window() const;
    bool has_pending_outbound() const noexcept;

    void enqueue_tcp_write(std::vector<char> payload);
    void flush_tcp_writes();
    void mark_payload_consumed(std::size_t bytes);
    void shutdown_tcp_send_if_ready();

    void send_local_fin();
    void maybe_finish();
    void schedule_close_after_fin();
    void fail_and_close(std::string_view where, const boost::system::error_code& error);

    asio::io_context& io_;
    std::weak_ptr<SmuxSession> session_;
    std::uint32_t id_ = 0;
    BaseConfig config_;
    tcp::socket tcp_socket_;
#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
    std::unique_ptr<boost::asio::local::stream_protocol::socket> local_socket_;
#endif
    tcp::resolver resolver_;
    asio::steady_timer close_timer_;
    QppStream qpp_;

    std::array<char, 32 * 1024> tcp_read_buffer_ {};
    std::deque<PendingBuffer> pending_outbound_;
    std::deque<std::vector<char>> pending_tcp_writes_;

    std::uint32_t num_read_ = 0;
    std::uint32_t num_written_ = 0;
    std::uint32_t incr_ = 0;
    std::uint32_t peer_consumed_ = 0;
    std::uint32_t peer_window_ = 262144;

    bool tcp_ready_ = false;
    bool tcp_write_active_ = false;
    bool local_fin_sent_ = false;
    bool remote_fin_received_ = false;
    bool closed_ = false;
    bool server_connecting_ = false;
};

class SmuxSession final : public std::enable_shared_from_this<SmuxSession> {
public:
    using AcceptHandler = std::function<void(std::shared_ptr<SmuxStream>)>;
    using CloseHandler = std::function<void()>;

    SmuxSession(asio::io_context& io,
                std::shared_ptr<KcpTransport> transport,
                BaseConfig config,
                bool client,
                CloseHandler close_handler = {});

    void start();
    void set_accept_handler(AcceptHandler handler);
    std::shared_ptr<SmuxStream> open_stream();
    void on_transport_bytes(std::span<const char> bytes);
    void close();

    bool is_closed() const noexcept { return closed_; }
    int version() const noexcept { return config_.smuxver; }
    int frame_size() const noexcept { return config_.framesize; }
    int stream_buffer() const noexcept { return config_.streambuf; }
    const udp::endpoint& remote_endpoint() const noexcept { return transport_->remote_endpoint(); }

private:
    friend class SmuxStream;

    enum class Command : std::uint8_t {
        syn = 0,
        fin = 1,
        psh = 2,
        nop = 3,
        upd = 4,
    };

    void send_frame(Command command, std::uint32_t stream_id, std::span<const char> payload = {});
    void send_update(std::uint32_t stream_id, std::uint32_t consumed, std::uint32_t window);
    void stream_closed(std::uint32_t stream_id);
    void consume_stream_bytes(std::span<const char> bytes);
    void start_keepalive_timer();
    void keepalive_tick();
    void protocol_error(std::string_view message);

    asio::io_context& io_;
    std::shared_ptr<KcpTransport> transport_;
    BaseConfig config_;
    bool client_ = false;
    CloseHandler close_handler_;
    AcceptHandler accept_handler_;
    asio::steady_timer keepalive_timer_;
    SnappyStreamEncoder compressor_;
    SnappyStreamDecoder decompressor_;
    std::shared_ptr<const QppPad> qpp_pad_;
    std::unordered_map<std::uint32_t, std::shared_ptr<SmuxStream>> streams_;
    std::vector<char> receive_buffer_;
    std::chrono::steady_clock::time_point last_activity_;
    std::uint32_t next_stream_id_ = 0;
    bool closed_ = false;
};

} // namespace shimakaze
