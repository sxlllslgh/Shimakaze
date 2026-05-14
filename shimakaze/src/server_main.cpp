#include "shimakaze/config.hpp"
#include "shimakaze/diagnostics.hpp"
#include "shimakaze/logger.hpp"
#include "shimakaze/server.hpp"
#include "shimakaze/stats.hpp"

#include <boost/asio.hpp>

#include <exception>
#include <iostream>

int main(int argc, char* argv[])
{
    using namespace shimakaze;

    try {
        auto config = parse_server_config(argc, argv);
        Logger::instance().set_quiet(config.quiet);
        Logger::instance().set_level(config.loglevel);
        Logger::instance().set_file(config.log);
        log_effective_server_config(config);
        log_compatibility_notes(config, false);

        asio::io_context io;
        auto snmp_logger = start_snmp_logger(io, config.snmplog, config.snmpperiod);
        auto diagnostics = start_diagnostics_server(io, config.pprof);
        (void)snmp_logger;
        (void)diagnostics;

        boost::asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&io](const boost::system::error_code&, int) {
            Logger::instance().info("signal received, stopping");
            io.stop();
        });

        Server server(io, std::move(config));
        server.start();
        io.run();
        return 0;
    } catch (const HelpRequested&) {
        std::cout << server_usage(argv[0]);
        return 0;
    } catch (const VersionRequested&) {
        std::cout << version << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "server: " << error.what() << '\n';
        std::cerr << server_usage(argv[0]);
        return 1;
    }
}
