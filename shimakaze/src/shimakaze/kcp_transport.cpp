#include "shimakaze/kcp_transport.hpp"

#include "shimakaze/logger.hpp"
#include "shimakaze/stats.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

extern "C" {
#include <ikcp.h>
}

namespace shimakaze {
namespace {

std::uint32_t read_le32_kcp(const char* data)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::uint64_t count_kcp_segments(std::span<const char> packet)
{
    constexpr std::size_t header_size = 24;
    std::size_t offset = 0;
    std::uint64_t count = 0;
    while (packet.size() >= offset + header_size) {
        const auto payload_size = read_le32_kcp(packet.data() + offset + 20);
        offset += header_size + payload_size;
        ++count;
        if (offset > packet.size()) {
            break;
        }
    }
    return count;
}

} // namespace

KcpTransport::KcpTransport(asio::io_context& io,
                           udp::socket& udp_socket,
                           udp::endpoint remote_endpoint,
                           std::uint32_t conv,
                           BaseConfig config,
                           CloseHandler close_handler)
    : io_(io)
    , udp_socket_(udp_socket)
    , remote_endpoint_(std::move(remote_endpoint))
    , update_timer_(io)
    , rate_timer_(io)
    , conv_(conv)
    , config_(std::move(config))
    , crypt_(config_)
    , fec_(config_)
    , close_handler_(std::move(close_handler))
{
    kcp_ = ikcp_create(conv_, this);
    if (kcp_ == nullptr) {
        throw std::runtime_error("ikcp_create failed");
    }
    configure_kcp();
}

KcpTransport::~KcpTransport()
{
    if (kcp_ != nullptr) {
        ikcp_release(kcp_);
        kcp_ = nullptr;
    }
}

void KcpTransport::set_read_handler(ReadHandler handler)
{
    read_handler_ = std::move(handler);
}

void KcpTransport::start()
{
    start_update_timer();
}

void KcpTransport::configure_kcp()
{
    ikcp_setoutput(kcp_, &KcpTransport::output_callback);
    ikcp_nodelay(kcp_, config_.nodelay, config_.interval, config_.resend, config_.nc);
    ikcp_wndsize(kcp_, config_.sndwnd, config_.rcvwnd);
    const auto kcp_mtu = config_.mtu - static_cast<int>(crypt_.overhead() + fec_.overhead());
    if (kcp_mtu < 50 || ikcp_setmtu(kcp_, kcp_mtu) != 0) {
        throw std::runtime_error("mtu is too small after Shimakaze packet overhead is reserved");
    }
    kcp_->stream = 1;
}

void KcpTransport::input(std::span<const char> packet)
{
    if (closing_ || packet.empty()) {
        return;
    }
    auto kcp_packets = fec_.decode(packet);
    for (const auto& kcp_packet : kcp_packets) {
        snmp_add(SnmpField::in_segs, count_kcp_segments(std::span<const char>(kcp_packet.data(), kcp_packet.size())));
        const auto result = ikcp_input(kcp_, kcp_packet.data(), static_cast<long>(kcp_packet.size()));
        if (result < 0) {
            snmp_add(SnmpField::kcp_in_errors);
            Logger::instance().warn("ikcp_input failed conv=", conv_, " result=", result);
            continue;
        }
        drain_kcp();
    }
    if (config_.acknodelay) {
        ikcp_flush(kcp_);
    }
}

void KcpTransport::send(std::span<const char> bytes)
{
    if (closing_ || bytes.empty()) {
        return;
    }
    snmp_add(SnmpField::bytes_sent, bytes.size());
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const auto result = ikcp_send(kcp_, bytes.data() + offset, static_cast<int>(remaining));
        if (result <= 0) {
            Logger::instance().warn("ikcp_send failed conv=", conv_, " result=", result);
            return;
        }
        offset += static_cast<std::size_t>(result);
    }
    ikcp_flush(kcp_);
}

void KcpTransport::close()
{
    if (closing_) {
        return;
    }
    closing_ = true;
    update_timer_.cancel();
    rate_timer_.cancel();
    if (close_handler_) {
        close_handler_();
    }
}

int KcpTransport::output_callback(const char* buffer, int length, ikcpcb*, void* user)
{
    auto* transport = static_cast<KcpTransport*>(user);
    transport->send_udp(buffer, length);
    return 0;
}

void KcpTransport::start_update_timer()
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

void KcpTransport::update_kcp()
{
    if (closing_) {
        return;
    }
    ikcp_update(kcp_, current_millis());
    drain_kcp();
    start_update_timer();
}

void KcpTransport::drain_kcp()
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
        snmp_add(SnmpField::bytes_received, buffer.size());
        if (read_handler_) {
            read_handler_(std::span<const char>(buffer.data(), buffer.size()));
        }
    }
}

void KcpTransport::send_udp(const char* buffer, int length)
{
    if (closing_ || length <= 0) {
        return;
    }
    snmp_add(SnmpField::out_segs, count_kcp_segments(std::span<const char>(buffer, static_cast<std::size_t>(length))));

    const auto fec_packets = fec_.encode(std::span<const char>(buffer, static_cast<std::size_t>(length)));
    for (const auto& fec_packet : fec_packets) {
        std::shared_ptr<std::vector<char>> payload;
        try {
            payload = std::make_shared<std::vector<char>>(
                crypt_.encrypt(std::span<const char>(fec_packet.data(), fec_packet.size())));
        } catch (const std::exception& error) {
            Logger::instance().warn("packet encryption failed conv=", conv_, ": ", error.what());
            close();
            return;
        }

        enqueue_udp_packet(std::move(payload));
    }
}

void KcpTransport::enqueue_udp_packet(std::shared_ptr<std::vector<char>> payload)
{
    if (config_.ratelimit <= 0) {
        udp_socket_.async_send_to(boost::asio::buffer(*payload), remote_endpoint_,
            [payload](const boost::system::error_code& error, std::size_t bytes) {
                if (!error) {
                    snmp_add(SnmpField::out_pkts);
                    snmp_add(SnmpField::out_bytes, bytes);
                }
            });
        return;
    }

    pending_udp_.push_back(std::move(payload));
    schedule_rate_limited_send();
}

void KcpTransport::schedule_rate_limited_send()
{
    if (closing_ || rate_send_active_ || pending_udp_.empty()) {
        return;
    }

    rate_send_active_ = true;
    const auto now = std::chrono::steady_clock::now();
    if (next_tx_time_ < now) {
        next_tx_time_ = now;
    }

    auto self = shared_from_this();
    rate_timer_.expires_at(next_tx_time_);
    rate_timer_.async_wait([self](const boost::system::error_code& error) {
        self->rate_send_active_ = false;
        if (error || self->closing_ || self->pending_udp_.empty()) {
            return;
        }

        auto payload = self->pending_udp_.front();
        self->pending_udp_.pop_front();
        const auto bytes_per_second = std::max(1, self->config_.ratelimit);
        const auto delay_us = static_cast<std::int64_t>(
            (static_cast<long double>(payload->size()) * 1000000.0L) /
            static_cast<long double>(bytes_per_second));
        self->next_tx_time_ = std::max(self->next_tx_time_, std::chrono::steady_clock::now()) +
                              std::chrono::microseconds(std::max<std::int64_t>(1, delay_us));
        self->udp_socket_.async_send_to(boost::asio::buffer(*payload), self->remote_endpoint_,
            [payload](const boost::system::error_code& send_error, std::size_t bytes) {
                if (!send_error) {
                    snmp_add(SnmpField::out_pkts);
                    snmp_add(SnmpField::out_bytes, bytes);
                }
            });
        self->schedule_rate_limited_send();
    });
}

} // namespace shimakaze
