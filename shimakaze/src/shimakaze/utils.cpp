#include "shimakaze/utils.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <random>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

extern "C" {
#include <ikcp.h>
}

namespace shimakaze {
namespace {

std::uint16_t parse_port(std::string_view text)
{
    unsigned value = 0;
    const auto* first = text.data();
    const auto* last = first + text.size();
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc {} || result.ptr != last || value == 0 || value > 65535) {
        throw std::runtime_error("invalid port: " + std::string(text));
    }
    return static_cast<std::uint16_t>(value);
}

std::pair<std::string_view, std::string_view> split_host_service(std::string_view address)
{
    if (address.empty()) {
        throw std::runtime_error("empty address");
    }

    if (address.front() == '[') {
        const auto close = address.find(']');
        if (close == std::string_view::npos || close + 1 >= address.size() || address[close + 1] != ':') {
            throw std::runtime_error("malformed bracketed IPv6 address: " + std::string(address));
        }
        return {address.substr(1, close - 1), address.substr(close + 2)};
    }

    const auto colon = address.rfind(':');
    if (colon == std::string_view::npos) {
        throw std::runtime_error("address must be host:port: " + std::string(address));
    }
    return {address.substr(0, colon), address.substr(colon + 1)};
}

template <typename Socket>
void set_buffers_impl(Socket& socket, int bytes)
{
    if (bytes <= 0) {
        return;
    }
    boost::system::error_code ignored;
    socket.set_option(boost::asio::socket_base::receive_buffer_size(bytes), ignored);
    socket.set_option(boost::asio::socket_base::send_buffer_size(bytes), ignored);
}

} // namespace

MultiPort parse_multiport(std::string_view address)
{
    const auto [host, service] = split_host_service(address);
    const auto dash = service.find('-');
    MultiPort result;
    result.host = std::string(host);

    if (dash == std::string_view::npos) {
        result.min_port = parse_port(service);
        result.max_port = result.min_port;
        return result;
    }

    result.min_port = parse_port(service.substr(0, dash));
    result.max_port = parse_port(service.substr(dash + 1));
    if (result.min_port > result.max_port) {
        throw std::runtime_error("invalid port range: " + std::string(service));
    }
    return result;
}

std::string join_host_port(std::string_view host, std::uint16_t port)
{
    if (host.find(':') != std::string_view::npos && !(host.starts_with('[') && host.ends_with(']'))) {
        return "[" + std::string(host) + "]:" + std::to_string(port);
    }
    return std::string(host) + ":" + std::to_string(port);
}

std::uint16_t choose_port(const MultiPort& ports)
{
    if (ports.min_port == ports.max_port) {
        return ports.min_port;
    }
    thread_local std::mt19937 generator {std::random_device {}()};
    std::uniform_int_distribution<int> distribution(ports.min_port, ports.max_port);
    return static_cast<std::uint16_t>(distribution(generator));
}

std::uint32_t random_conv()
{
    thread_local std::mt19937 generator {std::random_device {}()};
    std::uniform_int_distribution<std::uint32_t> distribution(1, UINT32_MAX);
    return distribution(generator);
}

std::uint32_t current_millis()
{
    using namespace std::chrono;
    const auto now = steady_clock::now().time_since_epoch();
    return static_cast<std::uint32_t>(duration_cast<milliseconds>(now).count());
}

std::uint32_t read_be32(const char* data)
{
    const auto bytes = reinterpret_cast<const unsigned char*>(data);
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) |
           static_cast<std::uint32_t>(bytes[3]);
}

void write_be32(char* data, std::uint32_t value)
{
    auto* bytes = reinterpret_cast<unsigned char*>(data);
    bytes[0] = static_cast<unsigned char>((value >> 24U) & 0xffU);
    bytes[1] = static_cast<unsigned char>((value >> 16U) & 0xffU);
    bytes[2] = static_cast<unsigned char>((value >> 8U) & 0xffU);
    bytes[3] = static_cast<unsigned char>(value & 0xffU);
}

std::string endpoint_key(const udp::endpoint& endpoint, std::uint32_t conv)
{
    return endpoint.address().to_string() + ":" + std::to_string(endpoint.port()) + "#" + std::to_string(conv);
}

udp::endpoint make_udp_bind_endpoint(asio::io_context& io, const MultiPort& address, std::uint16_t port)
{
    if (address.host.empty()) {
        return udp::endpoint(udp::v4(), port);
    }
    udp::resolver resolver(io);
    auto endpoints = resolver.resolve(address.host, std::to_string(port));
    if (endpoints.empty()) {
        throw std::runtime_error("failed to resolve bind address: " + join_host_port(address.host, port));
    }
    return *endpoints.begin();
}

tcp::endpoint make_tcp_bind_endpoint(asio::io_context& io, const MultiPort& address)
{
    if (address.host.empty()) {
        return tcp::endpoint(tcp::v4(), address.min_port);
    }
    tcp::resolver resolver(io);
    auto endpoints = resolver.resolve(address.host, std::to_string(address.min_port));
    if (endpoints.empty()) {
        throw std::runtime_error("failed to resolve bind address: " + join_host_port(address.host, address.min_port));
    }
    return *endpoints.begin();
}

udp::endpoint resolve_udp_endpoint(asio::io_context& io, std::string_view host, std::uint16_t port)
{
    udp::resolver resolver(io);
    auto endpoints = resolver.resolve(std::string(host), std::to_string(port));
    if (endpoints.empty()) {
        throw std::runtime_error("failed to resolve remote address: " + join_host_port(host, port));
    }
    return *endpoints.begin();
}

void set_socket_buffers(udp::socket& socket, int bytes)
{
    set_buffers_impl(socket, bytes);
}

void set_socket_buffers(tcp::socket& socket, int bytes)
{
    set_buffers_impl(socket, bytes);
}

void set_dscp(udp::socket& socket, int dscp)
{
    if (dscp <= 0) {
        return;
    }
    const auto clamped = std::clamp(dscp, 0, 63);
    const auto tos = clamped << 2;
    auto native = socket.native_handle();

#ifdef _WIN32
    const char ipv4_value = static_cast<char>(tos);
    ::setsockopt(native, IPPROTO_IP, IP_TOS, &ipv4_value, sizeof(ipv4_value));
#ifdef IPV6_TCLASS
    const char ipv6_value = static_cast<char>(clamped);
    ::setsockopt(native, IPPROTO_IPV6, IPV6_TCLASS, &ipv6_value, sizeof(ipv6_value));
#endif
#else
    int ipv4_value = tos;
    ::setsockopt(native, IPPROTO_IP, IP_TOS, &ipv4_value, sizeof(ipv4_value));
#ifdef IPV6_TCLASS
    int ipv6_value = clamped;
    ::setsockopt(native, IPPROTO_IPV6, IPV6_TCLASS, &ipv6_value, sizeof(ipv6_value));
#endif
#endif
}

} // namespace shimakaze
