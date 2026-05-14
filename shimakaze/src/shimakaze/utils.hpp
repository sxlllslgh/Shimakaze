#pragma once

#include <boost/asio.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace shimakaze {

namespace asio = boost::asio;
using boost::asio::ip::tcp;
using boost::asio::ip::udp;

struct MultiPort {
    std::string host;
    std::uint16_t min_port = 0;
    std::uint16_t max_port = 0;
};

MultiPort parse_multiport(std::string_view address);
std::string join_host_port(std::string_view host, std::uint16_t port);
std::uint16_t choose_port(const MultiPort& ports);
std::uint32_t random_conv();
std::uint32_t current_millis();

std::uint32_t read_be32(const char* data);
void write_be32(char* data, std::uint32_t value);

std::string endpoint_key(const udp::endpoint& endpoint, std::uint32_t conv);
udp::endpoint make_udp_bind_endpoint(asio::io_context& io, const MultiPort& address, std::uint16_t port);
tcp::endpoint make_tcp_bind_endpoint(asio::io_context& io, const MultiPort& address);
udp::endpoint resolve_udp_endpoint(asio::io_context& io, std::string_view host, std::uint16_t port);

void set_socket_buffers(udp::socket& socket, int bytes);
void set_socket_buffers(tcp::socket& socket, int bytes);
void set_dscp(udp::socket& socket, int dscp);

} // namespace shimakaze
