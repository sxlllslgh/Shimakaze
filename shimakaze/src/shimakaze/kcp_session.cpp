#include "shimakaze/kcp_session.hpp"

#include "shimakaze/logger.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

extern "C" {
#include <ikcp.h>
}

namespace shimakaze {
namespace {

constexpr std::size_t frame_header_size = 5;
constexpr std::uint32_t max_frame_payload = 16U * 1024U * 1024U;

} // namespace

KcpSession::KcpSession(asio::io_context& io,
                       udp::socket& udp_socket,
                       udp::endpoint remote_endpoint,
                       std::uint32_t conv,
                       BaseConfig config,
                       CloseHandler close_handler)
    : io_(io)
    , udp_socket_(udp_socket)
    , remote_endpoint_(std::move(remote_endpoint))
    , tcp_socket_(io)
    , resolver_(io)
    , update_timer_(io)
    , close_timer_(io)
    , conv_(conv)
    , config_(std::move(config))
    , crypt_(config_)
    , close_handler_(std::move(close_handler))
{
    kcp_ = ikcp_create(conv_, this);
    if (kcp_ == nullptr) {
        throw std::runtime_error("ikcp_create failed");
    }
    configure_kcp();
}

KcpSession::~KcpSession()
{
    if (kcp_ != nullptr) {
        ikcp_release(kcp_);
        kcp_ = nullptr;
    }
}

void KcpSession::configure_kcp()
{
    ikcp_setoutput(kcp_, &KcpSession::output_callback);
    ikcp_nodelay(kcp_, config_.nodelay, config_.interval, config_.resend, config_.nc);
    ikcp_wndsize(kcp_, config_.sndwnd, config_.rcvwnd);
    const auto packet_overhead = static_cast<int>(crypt_.overhead());
    const auto kcp_mtu = config_.mtu - packet_overhead;
    if (kcp_mtu < 50 || ikcp_setmtu(kcp_, kcp_mtu) != 0) {
        throw std::runtime_error("mtu is too small after Shimakaze packet overhead is reserved");
    }
    kcp_->stream = 1;
}

void KcpSession::start_client(tcp::socket tcp_socket)
{
    tcp_socket_ = std::move(tcp_socket);
    set_socket_buffers(tcp_socket_, config_.sockbuf);
    tcp_ready_ = true;
    Logger::instance().verbose("stream opened in: ", tcp_socket_.remote_endpoint(), " out: ", remote_endpoint_, " conv=", conv_);
    start_tcp_read();
    start_update_timer();
}

void KcpSession::start_server(std::string target_host, std::uint16_t target_port)
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
                    Logger::instance().verbose("stream opened in: ", self->remote_endpoint_, " out: ", self->tcp_socket_.remote_endpoint(), " conv=", self->conv_);
                    self->flush_tcp_writes();
                    self->start_tcp_read();
                });
        });
    start_update_timer();
}

void KcpSession::input(std::span<const char> packet)
{
    if (closing_ || packet.empty()) {
        return;
    }
    const auto result = ikcp_input(kcp_, packet.data(), static_cast<long>(packet.size()));
    if (result < 0) {
        Logger::instance().warn("ikcp_input failed conv=", conv_, " result=", result);
        return;
    }
    drain_kcp();
    if (config_.acknodelay) {
        ikcp_flush(kcp_);
    }
}

void KcpSession::close()
{
    auto keep_alive = shared_from_this();
    if (closing_) {
        return;
    }
    closing_ = true;

    boost::system::error_code ignored;
    update_timer_.cancel();
    close_timer_.cancel();
    resolver_.cancel();
    if (tcp_socket_.is_open()) {
        tcp_socket_.shutdown(tcp::socket::shutdown_both, ignored);
        tcp_socket_.close(ignored);
    }

    Logger::instance().verbose("stream closed conv=", conv_, " remote=", remote_endpoint_);
    if (close_handler_) {
        close_handler_(conv_);
    }
}

int KcpSession::output_callback(const char* buffer, int length, ikcpcb*, void* user)
{
    auto* session = static_cast<KcpSession*>(user);
    session->send_udp(buffer, length);
    return 0;
}

void KcpSession::start_update_timer()
{
    if (closing_) {
        return;
    }
    const auto interval = std::max(1, config_.interval);
    auto self = shared_from_this();
    update_timer_.expires_after(std::chrono::milliseconds(interval));
    update_timer_.async_wait([self](const boost::system::error_code& error) {
        if (!error) {
            self->update_kcp();
        }
    });
}

void KcpSession::update_kcp()
{
    if (closing_) {
        return;
    }
    ikcp_update(kcp_, current_millis());
    drain_kcp();
    start_update_timer();
}

void KcpSession::send_udp(const char* buffer, int length)
{
    if (closing_ || length <= 0) {
        return;
    }
    auto payload = std::make_shared<std::vector<char>>(
        crypt_.encrypt(std::span<const char>(buffer, static_cast<std::size_t>(length))));
    udp_socket_.async_send_to(boost::asio::buffer(*payload), remote_endpoint_,
        [payload](const boost::system::error_code&, std::size_t) {});
}

void KcpSession::start_tcp_read()
{
    if (closing_ || !tcp_ready_ || local_fin_sent_) {
        return;
    }

    auto self = shared_from_this();
    tcp_socket_.async_read_some(boost::asio::buffer(tcp_read_buffer_),
        [self](const boost::system::error_code& error, std::size_t bytes) {
            if (!error) {
                self->send_frame(FrameType::data, std::span<const char>(self->tcp_read_buffer_.data(), bytes));
                self->start_tcp_read();
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

void KcpSession::send_frame(FrameType type, std::span<const char> payload)
{
    if (closing_) {
        return;
    }
    if (payload.size() > max_frame_payload) {
        throw std::runtime_error("frame payload is too large");
    }

    std::vector<char> frame(frame_header_size + payload.size());
    frame[0] = static_cast<char>(type);
    write_be32(frame.data() + 1, static_cast<std::uint32_t>(payload.size()));
    if (!payload.empty()) {
        std::memcpy(frame.data() + frame_header_size, payload.data(), payload.size());
    }

    const auto result = ikcp_send(kcp_, frame.data(), static_cast<int>(frame.size()));
    if (result < 0) {
        Logger::instance().warn("ikcp_send failed conv=", conv_, " result=", result);
        return;
    }
    ikcp_flush(kcp_);
}

void KcpSession::drain_kcp()
{
    while (!closing_) {
        const auto size = ikcp_peeksize(kcp_);
        if (size <= 0) {
            break;
        }

        std::vector<char> buffer(static_cast<std::size_t>(size));
        const auto received = ikcp_recv(kcp_, buffer.data(), size);
        if (received <= 0) {
            break;
        }
        buffer.resize(static_cast<std::size_t>(received));
        kcp_stream_buffer_.insert(kcp_stream_buffer_.end(), buffer.begin(), buffer.end());
        parse_frames();
    }
}

void KcpSession::parse_frames()
{
    while (kcp_stream_buffer_.size() >= frame_header_size && !closing_) {
        const auto type = static_cast<std::uint8_t>(kcp_stream_buffer_[0]);
        const auto length = read_be32(kcp_stream_buffer_.data() + 1);
        if (length > max_frame_payload) {
            Logger::instance().warn("oversized frame conv=", conv_, " length=", length);
            close();
            return;
        }
        if (kcp_stream_buffer_.size() < frame_header_size + length) {
            return;
        }

        const auto payload_first = kcp_stream_buffer_.data() + frame_header_size;
        if (type == static_cast<std::uint8_t>(FrameType::data)) {
            handle_data_frame(std::span<const char>(payload_first, length));
        } else if (type == static_cast<std::uint8_t>(FrameType::fin)) {
            handle_fin_frame();
        } else {
            Logger::instance().warn("unknown frame type conv=", conv_, " type=", static_cast<int>(type));
            close();
            return;
        }

        kcp_stream_buffer_.erase(kcp_stream_buffer_.begin(),
                                 kcp_stream_buffer_.begin() + static_cast<std::ptrdiff_t>(frame_header_size + length));
    }
}

void KcpSession::handle_data_frame(std::span<const char> payload)
{
    if (payload.empty()) {
        return;
    }
    enqueue_tcp_write(std::vector<char>(payload.begin(), payload.end()));
}

void KcpSession::handle_fin_frame()
{
    if (remote_fin_received_) {
        return;
    }
    remote_fin_received_ = true;
    boost::system::error_code ignored;
    if (tcp_socket_.is_open()) {
        tcp_socket_.shutdown(tcp::socket::shutdown_send, ignored);
    }
    maybe_finish();
}

void KcpSession::enqueue_tcp_write(std::vector<char> data)
{
    pending_tcp_writes_.push_back(std::move(data));
    flush_tcp_writes();
}

void KcpSession::flush_tcp_writes()
{
    if (closing_ || !tcp_ready_ || tcp_write_active_ || pending_tcp_writes_.empty()) {
        return;
    }

    tcp_write_active_ = true;
    auto self = shared_from_this();
    boost::asio::async_write(tcp_socket_, boost::asio::buffer(pending_tcp_writes_.front()),
        [self](const boost::system::error_code& error, std::size_t) {
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
            self->flush_tcp_writes();
            self->maybe_finish();
        });
}

void KcpSession::send_local_fin()
{
    if (local_fin_sent_ || closing_) {
        return;
    }
    local_fin_sent_ = true;
    send_frame(FrameType::fin);
}

void KcpSession::maybe_finish()
{
    if (closing_) {
        return;
    }
    if (local_fin_sent_ && remote_fin_received_ && pending_tcp_writes_.empty() && !tcp_write_active_) {
        schedule_close_after_fin();
    }
}

void KcpSession::schedule_close_after_fin()
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

void KcpSession::fail_and_close(std::string_view where, const boost::system::error_code& error)
{
    Logger::instance().warn(where, " conv=", conv_, ": ", error.message());
    close();
}

} // namespace shimakaze
