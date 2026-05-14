#include "shimakaze/server.hpp"

#include "shimakaze/logger.hpp"
#include "shimakaze/stats.hpp"

#include <stdexcept>

extern "C" {
#include <ikcp.h>
}

namespace shimakaze {

Server::Server(asio::io_context& io, ServerConfig config)
    : io_(io)
    , config_(std::move(config))
    , crypt_(config_)
    , listen_address_(parse_multiport(config_.listen))
{
    bool parsed_tcp_target = false;
    try {
        target_address_ = parse_multiport(config_.target);
        parsed_tcp_target = true;
    } catch (const std::exception&) {
        target_is_unix_ = true;
        target_path_ = config_.target;
#if !defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
        throw std::runtime_error("Unix domain sockets are not supported by this platform build");
#endif
    }
    if (parsed_tcp_target && target_address_.min_port != target_address_.max_port) {
        throw std::runtime_error("server target must be a single TCP endpoint");
    }
}

void Server::start()
{
    for (auto port = listen_address_.min_port; port <= listen_address_.max_port; ++port) {
        auto listener = std::make_unique<UdpListener>(*this, port);
        listener->start();
        listeners_.push_back(std::move(listener));
        if (port == 65535) {
            break;
        }
    }
}

Server::UdpListener::UdpListener(Server& server, std::uint16_t port)
    : server_(server)
    , port_(port)
    , socket_(server.io_)
{
}

void Server::UdpListener::start()
{
    const auto endpoint = make_udp_bind_endpoint(server_.io_, server_.listen_address_, port_);
    socket_.open(endpoint.protocol());
    socket_.set_option(boost::asio::socket_base::reuse_address(true));
    socket_.bind(endpoint);
    set_socket_buffers(socket_, server_.config_.sockbuf);
    set_dscp(socket_, server_.config_.dscp);
    Logger::instance().info("listening UDP on: ", socket_.local_endpoint());
    start_receive();
}

void Server::UdpListener::start_receive()
{
    socket_.async_receive_from(boost::asio::buffer(buffer_), sender_,
        [this](const boost::system::error_code& error, std::size_t bytes) {
            if (!error) {
                snmp_add(SnmpField::in_pkts);
                snmp_add(SnmpField::in_bytes, bytes);
                handle_packet(bytes);
            } else if (error != boost::asio::error::operation_aborted) {
                snmp_add(SnmpField::in_errs);
                Logger::instance().warn("udp receive: ", error.message());
            }
            start_receive();
        });
}

void Server::UdpListener::handle_packet(std::size_t bytes)
{
    auto decoded = server_.crypt_.decrypt(std::span<const char>(buffer_.data(), bytes));
    if (!decoded || decoded->size() < 4) {
        snmp_add(SnmpField::in_csum_errors);
        return;
    }
    const auto offset = fec_kcp_payload_offset(std::span<const char>(decoded->data(), decoded->size()));
    if (!offset) {
        SessionEntry* target = nullptr;
        for (auto& [key, entry] : sessions_) {
            (void)key;
            if (entry.transport->remote_endpoint() == sender_) {
                if (target != nullptr) {
                    return;
                }
                target = &entry;
            }
        }
        if (target != nullptr) {
            target->transport->input(std::span<const char>(decoded->data(), decoded->size()));
        }
        return;
    }
    if (decoded->size() < *offset + 4) {
        return;
    }

    const auto conv = ikcp_getconv(decoded->data() + *offset);
    const auto key = endpoint_key(sender_, conv);
    auto found = sessions_.find(key);
    if (found == sessions_.end()) {
        auto transport = std::make_shared<KcpTransport>(
            server_.io_,
            socket_,
            sender_,
            conv,
            server_.config_,
            [this, key] {
                sessions_.erase(key);
                snmp_connection_close();
            });
        auto smux = std::make_shared<SmuxSession>(server_.io_, transport, server_.config_, false);
        smux->set_accept_handler([this](std::shared_ptr<SmuxStream> stream) {
#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
            if (server_.target_is_unix_) {
                stream->start_server_unix(server_.target_path_);
                return;
            }
#endif
            stream->start_server(server_.target_address_.host, server_.target_address_.min_port);
        });
        found = sessions_.emplace(key, SessionEntry {transport, smux}).first;
        snmp_connection_open(false);
        smux->start();
        Logger::instance().info("smux version: ", server_.config_.smuxver, " on connection: ", socket_.local_endpoint(), " <- ", sender_, " conv=", conv);
    }
    found->second.transport->input(std::span<const char>(decoded->data(), decoded->size()));
}

} // namespace shimakaze
