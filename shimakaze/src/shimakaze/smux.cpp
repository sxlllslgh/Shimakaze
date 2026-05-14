#include "shimakaze/smux.hpp"

#include "shimakaze/logger.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace shimakaze {
namespace {

constexpr std::size_t smux_header_size = 8;
constexpr std::size_t smux_update_size = 8;
constexpr std::chrono::seconds smux_timeout {30};

void write_le16(char* data, std::uint16_t value)
{
    auto* bytes = reinterpret_cast<unsigned char*>(data);
    bytes[0] = static_cast<unsigned char>(value & 0xffU);
    bytes[1] = static_cast<unsigned char>((value >> 8U) & 0xffU);
}

void write_le32(char* data, std::uint32_t value)
{
    auto* bytes = reinterpret_cast<unsigned char*>(data);
    bytes[0] = static_cast<unsigned char>(value & 0xffU);
    bytes[1] = static_cast<unsigned char>((value >> 8U) & 0xffU);
    bytes[2] = static_cast<unsigned char>((value >> 16U) & 0xffU);
    bytes[3] = static_cast<unsigned char>((value >> 24U) & 0xffU);
}

std::uint16_t read_le16(const char* data)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::uint32_t read_le32(const char* data)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

} // namespace

SmuxStream::SmuxStream(asio::io_context& io,
                       std::weak_ptr<SmuxSession> session,
                       std::uint32_t id,
                       BaseConfig config,
                       std::shared_ptr<const QppPad> qpp_pad)
    : io_(io)
    , session_(std::move(session))
    , id_(id)
    , config_(std::move(config))
    , tcp_socket_(io)
    , resolver_(io)
    , close_timer_(io)
    , qpp_(std::move(qpp_pad), config_.key)
{
}

void SmuxStream::start_client(tcp::socket tcp_socket)
{
    tcp_socket_ = std::move(tcp_socket);
    set_socket_buffers(tcp_socket_, config_.sockbuf);
    tcp_ready_ = true;
    if (!config_.quiet) {
        Logger::instance().verbose("stream opened in: ", tcp_socket_.remote_endpoint(), " out: smux(", id_, ")");
    }
    flush_tcp_writes();
    start_tcp_read();
}

#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
void SmuxStream::start_client(boost::asio::local::stream_protocol::socket local_socket)
{
    local_socket_ = std::make_unique<boost::asio::local::stream_protocol::socket>(std::move(local_socket));
    tcp_ready_ = true;
    if (!config_.quiet) {
        Logger::instance().verbose("stream opened in: unix out: smux(", id_, ")");
    }
    flush_tcp_writes();
    start_tcp_read();
}
#endif

void SmuxStream::start_server(std::string target_host, std::uint16_t target_port)
{
    server_connecting_ = true;
    auto self = shared_from_this();
    resolver_.async_resolve(std::move(target_host), std::to_string(target_port),
        [self](const boost::system::error_code& error, tcp::resolver::results_type endpoints) {
            if (error) {
                self->fail_and_close("resolve target", error);
                return;
            }

            boost::asio::async_connect(self->tcp_socket_, endpoints,
                [self](const boost::system::error_code& connect_error, const tcp::endpoint&) {
                    self->server_connecting_ = false;
                    if (connect_error) {
                        self->fail_and_close("connect target", connect_error);
                        return;
                    }

                    set_socket_buffers(self->tcp_socket_, self->config_.sockbuf);
                    self->tcp_ready_ = true;
                    if (!self->config_.quiet) {
                        Logger::instance().verbose("stream opened in: smux(", self->id_, ") out: ", self->tcp_socket_.remote_endpoint());
                    }
                    self->flush_tcp_writes();
                    self->start_tcp_read();
                    self->shutdown_tcp_send_if_ready();
                });
        });
}

#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
void SmuxStream::start_server_unix(std::string target_path)
{
    server_connecting_ = true;
    local_socket_ = std::make_unique<boost::asio::local::stream_protocol::socket>(io_);
    auto self = shared_from_this();
    local_socket_->async_connect(boost::asio::local::stream_protocol::endpoint(std::move(target_path)),
        [self](const boost::system::error_code& connect_error) {
            self->server_connecting_ = false;
            if (connect_error) {
                self->fail_and_close("connect unix target", connect_error);
                return;
            }

            self->tcp_ready_ = true;
            if (!self->config_.quiet) {
                Logger::instance().verbose("stream opened in: smux(", self->id_, ") out: unix");
            }
            self->flush_tcp_writes();
            self->start_tcp_read();
            self->shutdown_tcp_send_if_ready();
        });
}
#endif

void SmuxStream::start_tcp_read()
{
    if (closed_ || !tcp_ready_ || local_fin_sent_ || has_pending_outbound()) {
        return;
    }

    auto self = shared_from_this();
#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
    if (local_socket_) {
        local_socket_->async_read_some(boost::asio::buffer(tcp_read_buffer_),
            [self](const boost::system::error_code& error, std::size_t bytes) {
                if (!error) {
                    self->queue_outbound(std::span<const char>(self->tcp_read_buffer_.data(), bytes));
                    self->try_flush_outbound();
                    if (!self->has_pending_outbound()) {
                        self->start_tcp_read();
                    }
                    return;
                }

                if (error == boost::asio::error::eof ||
                    error == boost::asio::error::connection_reset ||
                    error == boost::asio::error::operation_aborted) {
                    self->send_local_fin();
                    self->maybe_finish();
                    return;
                }

                self->fail_and_close("unix read", error);
            });
        return;
    }
#endif
    tcp_socket_.async_read_some(boost::asio::buffer(tcp_read_buffer_),
        [self](const boost::system::error_code& error, std::size_t bytes) {
            if (!error) {
                self->queue_outbound(std::span<const char>(self->tcp_read_buffer_.data(), bytes));
                self->try_flush_outbound();
                if (!self->has_pending_outbound()) {
                    self->start_tcp_read();
                }
                return;
            }

            if (error == boost::asio::error::eof ||
                error == boost::asio::error::connection_reset ||
                error == boost::asio::error::operation_aborted) {
                self->send_local_fin();
                self->maybe_finish();
                return;
            }

            self->fail_and_close("tcp read", error);
        });
}

void SmuxStream::queue_outbound(std::span<const char> payload)
{
    if (payload.empty()) {
        return;
    }
    PendingBuffer pending;
    pending.data.assign(payload.begin(), payload.end());
    pending_outbound_.push_back(std::move(pending));
}

void SmuxStream::try_flush_outbound()
{
    auto session = session_.lock();
    if (!session || closed_) {
        return;
    }

    const auto max_frame = static_cast<std::size_t>(std::max(1, session->frame_size()));
    while (!pending_outbound_.empty()) {
        auto& front = pending_outbound_.front();
        const auto remaining = front.data.size() - front.offset;
        if (remaining == 0) {
            pending_outbound_.pop_front();
            continue;
        }

        std::size_t allowed = remaining;
        if (session->version() == 2) {
            const auto window = available_peer_window();
            if (window <= 0) {
                return;
            }
            allowed = std::min<std::size_t>(allowed, static_cast<std::size_t>(window));
        }
        const auto chunk_size = std::min(allowed, max_frame);
        auto chunk = std::span<const char>(front.data.data() + front.offset, chunk_size);
        if (qpp_.enabled()) {
            std::vector<char> encrypted(chunk.begin(), chunk.end());
            qpp_.encrypt(std::span<char>(encrypted.data(), encrypted.size()));
            session->send_frame(SmuxSession::Command::psh,
                                id_,
                                std::span<const char>(encrypted.data(), encrypted.size()));
        } else {
            session->send_frame(SmuxSession::Command::psh, id_, chunk);
        }
        front.offset += chunk_size;
        num_written_ += static_cast<std::uint32_t>(chunk_size);
    }
}

std::int64_t SmuxStream::available_peer_window() const
{
    const auto inflight = static_cast<std::uint32_t>(num_written_ - peer_consumed_);
    if (static_cast<std::int32_t>(inflight) < 0) {
        return 0;
    }
    return static_cast<std::int64_t>(peer_window_) - static_cast<std::int64_t>(inflight);
}

bool SmuxStream::has_pending_outbound() const noexcept
{
    return !pending_outbound_.empty();
}

void SmuxStream::on_data(std::span<const char> payload)
{
    if (closed_ || payload.empty()) {
        return;
    }
    std::vector<char> decoded(payload.begin(), payload.end());
    qpp_.decrypt(std::span<char>(decoded.data(), decoded.size()));
    enqueue_tcp_write(std::move(decoded));
}

void SmuxStream::enqueue_tcp_write(std::vector<char> payload)
{
    pending_tcp_writes_.push_back(std::move(payload));
    flush_tcp_writes();
}

void SmuxStream::flush_tcp_writes()
{
    if (closed_ || !tcp_ready_ || tcp_write_active_ || pending_tcp_writes_.empty()) {
        return;
    }

    tcp_write_active_ = true;
    auto self = shared_from_this();
    auto handler = [self](const boost::system::error_code& error, std::size_t bytes) {
            self->tcp_write_active_ = false;
            if (error) {
                if (error == boost::asio::error::operation_aborted ||
                    error == boost::asio::error::connection_reset ||
                    error == boost::asio::error::broken_pipe) {
                    self->send_local_fin();
                    self->maybe_finish();
                    return;
                }
                self->fail_and_close("tcp write", error);
                return;
            }

            self->pending_tcp_writes_.pop_front();
            self->mark_payload_consumed(bytes);
            self->flush_tcp_writes();
            self->shutdown_tcp_send_if_ready();
            self->maybe_finish();
        };
#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
    if (local_socket_) {
        boost::asio::async_write(*local_socket_, boost::asio::buffer(pending_tcp_writes_.front()), std::move(handler));
        return;
    }
#endif
    boost::asio::async_write(tcp_socket_, boost::asio::buffer(pending_tcp_writes_.front()), std::move(handler));
}

void SmuxStream::mark_payload_consumed(std::size_t bytes)
{
    auto session = session_.lock();
    if (!session || session->version() != 2 || bytes == 0) {
        return;
    }

    num_read_ += static_cast<std::uint32_t>(bytes);
    incr_ += static_cast<std::uint32_t>(bytes);
    if (incr_ >= static_cast<std::uint32_t>(std::max(1, session->stream_buffer() / 2)) ||
        num_read_ == static_cast<std::uint32_t>(bytes)) {
        session->send_update(id_, num_read_, static_cast<std::uint32_t>(std::max(1, session->stream_buffer())));
        incr_ = 0;
    }
}

void SmuxStream::on_fin()
{
    if (remote_fin_received_) {
        return;
    }
    remote_fin_received_ = true;
    shutdown_tcp_send_if_ready();
    maybe_finish();
}

void SmuxStream::shutdown_tcp_send_if_ready()
{
    if (!remote_fin_received_ || tcp_write_active_ || !pending_tcp_writes_.empty() || !tcp_ready_) {
        return;
    }
    boost::system::error_code ignored;
#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
    if (local_socket_ && local_socket_->is_open()) {
        local_socket_->shutdown(boost::asio::socket_base::shutdown_send, ignored);
        return;
    }
#endif
    if (tcp_socket_.is_open()) {
        tcp_socket_.shutdown(tcp::socket::shutdown_send, ignored);
    }
}

void SmuxStream::on_update(std::uint32_t consumed, std::uint32_t window)
{
    peer_consumed_ = consumed;
    peer_window_ = window;
    try_flush_outbound();
    if (!has_pending_outbound()) {
        start_tcp_read();
    }
}

void SmuxStream::send_local_fin()
{
    if (local_fin_sent_ || closed_) {
        return;
    }
    local_fin_sent_ = true;
    if (auto session = session_.lock()) {
        session->send_frame(SmuxSession::Command::fin, id_);
    }
}

void SmuxStream::maybe_finish()
{
    if (closed_) {
        return;
    }
    if (local_fin_sent_ && remote_fin_received_ &&
        pending_tcp_writes_.empty() && !tcp_write_active_ &&
        pending_outbound_.empty()) {
        schedule_close_after_fin();
    }
}

void SmuxStream::schedule_close_after_fin()
{
    auto self = shared_from_this();
    const auto delay = std::chrono::seconds(std::max(0, config_.closewait));
    close_timer_.expires_after(delay);
    close_timer_.async_wait([self](const boost::system::error_code& error) {
        if (!error) {
            self->close();
        }
    });
}

void SmuxStream::fail_and_close(std::string_view where, const boost::system::error_code& error)
{
    Logger::instance().warn(where, " smux stream=", id_, ": ", error.message());
    close();
}

void SmuxStream::close()
{
    if (closed_) {
        return;
    }
    send_local_fin();
    closed_ = true;

    boost::system::error_code ignored;
    close_timer_.cancel();
    resolver_.cancel();
#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
    if (local_socket_ && local_socket_->is_open()) {
        local_socket_->shutdown(boost::asio::socket_base::shutdown_both, ignored);
        local_socket_->close(ignored);
    }
#endif
    if (tcp_socket_.is_open()) {
        tcp_socket_.shutdown(tcp::socket::shutdown_both, ignored);
        tcp_socket_.close(ignored);
    }

    if (!config_.quiet) {
        Logger::instance().verbose("stream closed smux(", id_, ")");
    }
    if (auto session = session_.lock()) {
        session->stream_closed(id_);
    }
}

SmuxSession::SmuxSession(asio::io_context& io,
                         std::shared_ptr<KcpTransport> transport,
                         BaseConfig config,
                         bool client,
                         CloseHandler close_handler)
    : io_(io)
    , transport_(std::move(transport))
    , config_(std::move(config))
    , client_(client)
    , close_handler_(std::move(close_handler))
    , keepalive_timer_(io)
    , last_activity_(std::chrono::steady_clock::now())
    , next_stream_id_(client ? 1U : 0U)
{
    if (config_.qpp) {
        qpp_pad_ = std::make_shared<QppPad>(config_.key, static_cast<std::uint16_t>(config_.qpp_count));
    }
}

void SmuxSession::start()
{
    auto weak = weak_from_this();
    transport_->set_read_handler([weak](std::span<const char> bytes) {
        if (auto self = weak.lock()) {
            self->on_transport_bytes(bytes);
        }
    });
    transport_->start();
    start_keepalive_timer();
}

void SmuxSession::set_accept_handler(AcceptHandler handler)
{
    accept_handler_ = std::move(handler);
}

std::shared_ptr<SmuxStream> SmuxSession::open_stream()
{
    if (closed_) {
        throw std::runtime_error("smux session is closed");
    }
    if (next_stream_id_ + 2 < next_stream_id_) {
        throw std::runtime_error("smux stream id exhausted");
    }

    next_stream_id_ += 2;
    const auto sid = next_stream_id_;
    auto stream = std::make_shared<SmuxStream>(io_, weak_from_this(), sid, config_, qpp_pad_);
    send_frame(Command::syn, sid);
    streams_.emplace(sid, stream);
    return stream;
}

void SmuxSession::on_transport_bytes(std::span<const char> bytes)
{
    if (closed_ || bytes.empty()) {
        return;
    }
    last_activity_ = std::chrono::steady_clock::now();
    if (!config_.nocomp) {
        try {
            auto decoded = decompressor_.decode(bytes);
            for (const auto& chunk : decoded) {
                consume_stream_bytes(std::span<const char>(chunk.data(), chunk.size()));
            }
        } catch (const std::exception& error) {
            protocol_error(error.what());
        }
        return;
    }

    consume_stream_bytes(bytes);
}

void SmuxSession::consume_stream_bytes(std::span<const char> bytes)
{
    if (receive_buffer_.size() + bytes.size() > static_cast<std::size_t>(std::max(1, config_.smuxbuf))) {
        protocol_error("smux receive buffer overflow");
        return;
    }
    receive_buffer_.insert(receive_buffer_.end(), bytes.begin(), bytes.end());

    while (!closed_ && receive_buffer_.size() >= smux_header_size) {
        const auto* header = receive_buffer_.data();
        const auto version_byte = static_cast<unsigned char>(header[0]);
        const auto command = static_cast<unsigned char>(header[1]);
        const auto length = read_le16(header + 2);
        const auto stream_id = read_le32(header + 4);

        if (version_byte != static_cast<unsigned char>(config_.smuxver)) {
            protocol_error("smux protocol version mismatch");
            return;
        }
        if (receive_buffer_.size() < smux_header_size + length) {
            return;
        }

        auto payload = std::span<const char>(receive_buffer_.data() + smux_header_size, length);
        switch (static_cast<Command>(command)) {
        case Command::nop:
            break;
        case Command::syn: {
            if (!streams_.contains(stream_id)) {
                auto stream = std::make_shared<SmuxStream>(io_, weak_from_this(), stream_id, config_, qpp_pad_);
                streams_.emplace(stream_id, stream);
                if (accept_handler_) {
                    accept_handler_(stream);
                }
            }
            break;
        }
        case Command::fin: {
            if (auto iter = streams_.find(stream_id); iter != streams_.end()) {
                iter->second->on_fin();
            }
            break;
        }
        case Command::psh: {
            if (auto iter = streams_.find(stream_id); iter != streams_.end()) {
                iter->second->on_data(payload);
            }
            break;
        }
        case Command::upd: {
            if (config_.smuxver != 2 || length != smux_update_size) {
                protocol_error("invalid smux update frame");
                return;
            }
            if (auto iter = streams_.find(stream_id); iter != streams_.end()) {
                iter->second->on_update(read_le32(payload.data()), read_le32(payload.data() + 4));
            }
            break;
        }
        default:
            protocol_error("invalid smux command");
            return;
        }

        receive_buffer_.erase(receive_buffer_.begin(),
                              receive_buffer_.begin() + static_cast<std::ptrdiff_t>(smux_header_size + length));
    }
}

void SmuxSession::send_frame(Command command, std::uint32_t stream_id, std::span<const char> payload)
{
    if (closed_ || payload.size() > static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max())) {
        return;
    }
    std::vector<char> frame(smux_header_size + payload.size());
    frame[0] = static_cast<char>(config_.smuxver);
    frame[1] = static_cast<char>(command);
    write_le16(frame.data() + 2, static_cast<std::uint16_t>(payload.size()));
    write_le32(frame.data() + 4, stream_id);
    if (!payload.empty()) {
        std::memcpy(frame.data() + smux_header_size, payload.data(), payload.size());
    }
    if (config_.nocomp) {
        transport_->send(std::span<const char>(frame.data(), frame.size()));
        return;
    }
    auto chunks = compressor_.encode(std::span<const char>(frame.data(), frame.size()));
    for (const auto& chunk : chunks) {
        transport_->send(std::span<const char>(chunk.data(), chunk.size()));
    }
}

void SmuxSession::send_update(std::uint32_t stream_id, std::uint32_t consumed, std::uint32_t window)
{
    std::array<char, smux_update_size> payload {};
    write_le32(payload.data(), consumed);
    write_le32(payload.data() + 4, window);
    send_frame(Command::upd, stream_id, std::span<const char>(payload.data(), payload.size()));
}

void SmuxSession::stream_closed(std::uint32_t stream_id)
{
    streams_.erase(stream_id);
}

void SmuxSession::start_keepalive_timer()
{
    if (closed_) {
        return;
    }
    auto self = shared_from_this();
    keepalive_timer_.expires_after(std::chrono::seconds(std::max(1, config_.keepalive)));
    keepalive_timer_.async_wait([self](const boost::system::error_code& error) {
        if (!error) {
            self->keepalive_tick();
        }
    });
}

void SmuxSession::keepalive_tick()
{
    if (closed_) {
        return;
    }
    send_frame(Command::nop, 0);
    if (std::chrono::steady_clock::now() - last_activity_ > smux_timeout) {
        close();
        return;
    }
    start_keepalive_timer();
}

void SmuxSession::protocol_error(std::string_view message)
{
    Logger::instance().warn(message, " remote=", transport_->remote_endpoint());
    close();
}

void SmuxSession::close()
{
    if (closed_) {
        return;
    }
    closed_ = true;
    keepalive_timer_.cancel();

    auto streams = std::move(streams_);
    for (auto& [id, stream] : streams) {
        (void)id;
        stream->close();
    }
    transport_->close();
    if (close_handler_) {
        close_handler_();
    }
}

} // namespace shimakaze
