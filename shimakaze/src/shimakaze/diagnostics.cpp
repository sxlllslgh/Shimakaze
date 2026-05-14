#include "shimakaze/diagnostics.hpp"

#include "shimakaze/logger.hpp"
#include "shimakaze/stats.hpp"

#include <array>
#include <sstream>
#include <string>
#include <utility>

namespace shimakaze {
namespace {

class DiagnosticsServer final : public DiagnosticsServerHandle, public std::enable_shared_from_this<DiagnosticsServer> {
public:
    explicit DiagnosticsServer(asio::io_context& io)
        : acceptor_(io)
    {
    }

    bool start()
    {
        boost::system::error_code error;
        const tcp::endpoint endpoint(tcp::v4(), 6060);
        acceptor_.open(endpoint.protocol(), error);
        if (error) {
            Logger::instance().warn("pprof server: ", error.message());
            return false;
        }
        acceptor_.set_option(boost::asio::socket_base::reuse_address(true), error);
        acceptor_.bind(endpoint, error);
        if (error) {
            Logger::instance().warn("pprof server: ", error.message());
            return false;
        }
        acceptor_.listen(boost::asio::socket_base::max_listen_connections, error);
        if (error) {
            Logger::instance().warn("pprof server: ", error.message());
            return false;
        }
        Logger::instance().info("pprof server listening on: 0.0.0.0:6060");
        accept();
        return true;
    }

private:
    class Connection final : public std::enable_shared_from_this<Connection> {
    public:
        explicit Connection(tcp::socket socket)
            : socket_(std::move(socket))
        {
        }

        void start()
        {
            auto self = shared_from_this();
            socket_.async_read_some(boost::asio::buffer(buffer_),
                [self](const boost::system::error_code& error, std::size_t bytes) {
                    if (!error) {
                        self->request_.append(self->buffer_.data(), bytes);
                        if (self->request_.find("\r\n\r\n") == std::string::npos &&
                            self->request_.size() < 8192) {
                            self->start();
                            return;
                        }
                    }
                    self->write_response();
                });
        }

    private:
        static std::string request_path(const std::string& request)
        {
            const auto first_space = request.find(' ');
            if (first_space == std::string::npos) {
                return "/";
            }
            const auto second_space = request.find(' ', first_space + 1);
            if (second_space == std::string::npos) {
                return "/";
            }
            return request.substr(first_space + 1, second_space - first_space - 1);
        }

        static std::string response_for(const std::string& path)
        {
            std::string body;
            std::string content_type = "text/plain; charset=utf-8";
            std::string status = "200 OK";

            if (path == "/debug/vars") {
                body = snmp_json();
                content_type = "application/json";
            } else if (path == "/debug/pprof/" || path == "/debug/pprof") {
                body = "Shimakaze diagnostics\n\n/debug/vars\n/debug/pprof/profile\n";
            } else if (path == "/debug/pprof/profile") {
                status = "501 Not Implemented";
                body = "CPU profiling is not available in this portable C++ build.\nUse /debug/vars for live SNMP counters.\n";
            } else {
                status = "404 Not Found";
                body = "not found\n";
            }

            std::ostringstream os;
            os << "HTTP/1.1 " << status << "\r\n"
               << "Content-Type: " << content_type << "\r\n"
               << "Content-Length: " << body.size() << "\r\n"
               << "Connection: close\r\n\r\n"
               << body;
            return os.str();
        }

        void write_response()
        {
            response_ = response_for(request_path(request_));
            auto self = shared_from_this();
            boost::asio::async_write(socket_, boost::asio::buffer(response_),
                [self](const boost::system::error_code&, std::size_t) {
                    boost::system::error_code ignored;
                    self->socket_.shutdown(tcp::socket::shutdown_both, ignored);
                    self->socket_.close(ignored);
                });
        }

        tcp::socket socket_;
        std::array<char, 2048> buffer_ {};
        std::string request_;
        std::string response_;
    };

    void accept()
    {
        acceptor_.async_accept([self = shared_from_this()](const boost::system::error_code& error, tcp::socket socket) {
            if (!error) {
                std::make_shared<Connection>(std::move(socket))->start();
            } else if (error != boost::asio::error::operation_aborted) {
                Logger::instance().warn("pprof accept: ", error.message());
            }
            if (self->acceptor_.is_open()) {
                self->accept();
            }
        });
    }

    tcp::acceptor acceptor_;
};

} // namespace

std::shared_ptr<DiagnosticsServerHandle> start_diagnostics_server(asio::io_context& io, bool enabled)
{
    if (!enabled) {
        return {};
    }
    auto server = std::make_shared<DiagnosticsServer>(io);
    if (!server->start()) {
        return {};
    }
    return server;
}

} // namespace shimakaze
