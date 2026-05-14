#include "shimakaze/client.hpp"

#include "shimakaze/logger.hpp"
#include "shimakaze/stats.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

extern "C" {
#include <ikcp.h>
}

namespace shimakaze {

Client::Client(asio::io_context& io, ClientConfig config)
    : io_(io)
    , config_(std::move(config))
    , crypt_(config_)
    , remote_address_(parse_multiport(config_.remoteaddr))
    , acceptor_(io)
    , muxes_(static_cast<std::size_t>(std::max(1, config_.conn)))
{
    bool parsed_tcp_local = false;
    try {
        local_address_ = parse_multiport(config_.localaddr);
        parsed_tcp_local = true;
    } catch (const std::exception&) {
        local_is_unix_ = true;
        local_path_ = config_.localaddr;
#if !defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
        throw std::runtime_error("Unix domain sockets are not supported by this platform build");
#endif
    }
    if (parsed_tcp_local && local_address_.min_port != local_address_.max_port) {
        throw std::runtime_error("client localaddr currently supports a single listen port");
    }
}

void Client::start()
{
#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
    if (local_is_unix_) {
        local_acceptor_ = std::make_unique<boost::asio::local::stream_protocol::acceptor>(io_);
        const boost::asio::local::stream_protocol::endpoint endpoint(local_path_);
        local_acceptor_->open(endpoint.protocol());
        local_acceptor_->bind(endpoint);
        local_acceptor_->listen(boost::asio::socket_base::max_listen_connections);
        Logger::instance().info("listening Unix on: ", local_path_);
        start_local_accept();
        return;
    }
#endif

    const auto listen_endpoint = make_tcp_bind_endpoint(io_, local_address_);

    acceptor_.open(listen_endpoint.protocol());
    acceptor_.set_option(boost::asio::socket_base::reuse_address(true));
    acceptor_.bind(listen_endpoint);
    acceptor_.listen(boost::asio::socket_base::max_listen_connections);

    Logger::instance().info("listening TCP on: ", acceptor_.local_endpoint());

    start_accept();
}

#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
void Client::start_local_accept()
{
    local_acceptor_->async_accept([this](const boost::system::error_code& error,
                                         boost::asio::local::stream_protocol::socket socket) {
        if (!error) {
            handle_accept(std::move(socket));
        } else {
            Logger::instance().warn("accept unix: ", error.message());
        }
        start_local_accept();
    });
}
#endif

void Client::start_accept()
{
    acceptor_.async_accept([this](const boost::system::error_code& error, tcp::socket socket) {
        if (!error) {
            handle_accept(std::move(socket));
        } else {
            Logger::instance().warn("accept: ", error.message());
        }
        start_accept();
    });
}

void Client::handle_accept(tcp::socket socket)
{
    try {
        const auto index = rr_++ % muxes_.size();
        auto smux = ensure_session(index);
        auto stream = smux->open_stream();
        stream->start_client(std::move(socket));
    } catch (const std::exception& error) {
        Logger::instance().warn("open smux stream: ", error.what());
    }
}

#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
void Client::handle_accept(boost::asio::local::stream_protocol::socket socket)
{
    try {
        const auto index = rr_++ % muxes_.size();
        auto smux = ensure_session(index);
        auto stream = smux->open_stream();
        stream->start_client(std::move(socket));
    } catch (const std::exception& error) {
        Logger::instance().warn("open smux stream: ", error.what());
    }
}
#endif

void Client::start_udp_receive(const std::shared_ptr<Connection>& connection)
{
    connection->socket.async_receive_from(boost::asio::buffer(connection->buffer), connection->sender,
        [this, connection](const boost::system::error_code& error, std::size_t bytes) {
            if (!error) {
                snmp_add(SnmpField::in_pkts);
                snmp_add(SnmpField::in_bytes, bytes);
                auto decoded = crypt_.decrypt(std::span<const char>(connection->buffer.data(), bytes));
                if (!decoded || decoded->size() < 4) {
                    snmp_add(SnmpField::in_csum_errors);
                    start_udp_receive(connection);
                    return;
                }
                const auto offset = fec_kcp_payload_offset(std::span<const char>(decoded->data(), decoded->size()));
                if (!offset) {
                    if (connection->transport && connection->transport->remote_endpoint() == connection->sender) {
                        connection->transport->input(std::span<const char>(decoded->data(), decoded->size()));
                    }
                } else if (connection->transport && decoded->size() >= *offset + 4) {
                    const auto conv = ikcp_getconv(decoded->data() + *offset);
                    if (conv == connection->conv && connection->transport->remote_endpoint() == connection->sender) {
                        connection->transport->input(std::span<const char>(decoded->data(), decoded->size()));
                    }
                }
            } else if (error != boost::asio::error::operation_aborted) {
                snmp_add(SnmpField::in_errs);
                Logger::instance().warn("udp receive: ", error.message());
            }
            if (connection->socket.is_open()) {
                start_udp_receive(connection);
            }
        });
}

std::shared_ptr<SmuxSession> Client::ensure_session(std::size_t index)
{
    auto& slot = muxes_.at(index);
    const auto now = std::chrono::steady_clock::now();
    const auto expired = config_.autoexpire > 0 && slot.connection && slot.connection->smux && now >= slot.connection->expiry;
    if (slot.connection && slot.connection->smux && !slot.connection->smux->is_closed() && !expired) {
        return slot.connection->smux;
    }

    auto connection = create_connection();
    slot.connection = connection;
    return connection->smux;
}

std::shared_ptr<Client::Connection> Client::create_connection()
{
    auto connection = std::make_shared<Connection>(io_);
    const auto conv = next_conv();
    const auto remote = pick_remote_endpoint();

    connection->socket.open(remote.protocol());
    connection->socket.bind(udp::endpoint(remote.protocol(), 0));
    set_socket_buffers(connection->socket, config_.sockbuf);
    set_dscp(connection->socket, config_.dscp);
    connection->conv = conv;

    std::weak_ptr<Connection> weak_connection = connection;
    auto transport = std::make_shared<KcpTransport>(
        io_,
        connection->socket,
        remote,
        conv,
        config_,
        [this, conv, weak_connection] {
            transports_.erase(conv);
            snmp_connection_close();
            if (auto locked = weak_connection.lock()) {
                boost::system::error_code ignored;
                locked->socket.close(ignored);
            }
        });
    auto smux = std::make_shared<SmuxSession>(io_, transport, config_, true);
    transports_.emplace(conv, transport);
    snmp_connection_open(true);
    smux->start();

    connection->transport = std::move(transport);
    connection->smux = std::move(smux);
    connection->expiry = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(0, config_.autoexpire));
    if (config_.autoexpire > 0) {
        schedule_scavenge(connection,
                          std::chrono::seconds(std::max(0, config_.autoexpire) +
                                               std::max(0, config_.scavengettl)));
    }

    Logger::instance().info("smux version: ", config_.smuxver, " on connection: ", connection->socket.local_endpoint(), " -> ", remote, " conv=", conv);
    start_udp_receive(connection);
    return connection;
}

void Client::schedule_scavenge(std::shared_ptr<Connection> connection, std::chrono::seconds delay)
{
    auto timer = std::make_shared<asio::steady_timer>(io_);
    scavenge_timers_.push_back(timer);
    timer->expires_after(delay);
    timer->async_wait([this, timer, connection = std::move(connection)](const boost::system::error_code& error) {
        const auto timer_pos = std::ranges::find(scavenge_timers_, timer);
        if (timer_pos != scavenge_timers_.end()) {
            scavenge_timers_.erase(timer_pos);
        }
        if (error) {
            return;
        }
        if (connection->smux) {
            connection->smux->close();
        } else if (connection->transport) {
            connection->transport->close();
        } else {
            boost::system::error_code ignored;
            connection->socket.close(ignored);
        }
    });
}

udp::endpoint Client::pick_remote_endpoint()
{
    const auto port = choose_port(remote_address_);
    return resolve_udp_endpoint(io_, remote_address_.host, port);
}

std::uint32_t Client::next_conv() const
{
    for (;;) {
        const auto conv = random_conv();
        if (!transports_.contains(conv)) {
            return conv;
        }
    }
}

} // namespace shimakaze
