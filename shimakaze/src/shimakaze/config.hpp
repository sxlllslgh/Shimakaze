#pragma once

#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace shimakaze {

struct BaseConfig {
    std::string key = "it's a secrect";
    std::string crypt = "aes";
    std::string mode = "fast";
    int mtu = 1350;
    int ratelimit = 0;
    int sndwnd = 128;
    int rcvwnd = 512;
    int datashard = 10;
    int parityshard = 3;
    int dscp = 0;
    bool nocomp = false;
    bool acknodelay = false;
    int nodelay = 0;
    int interval = 50;
    int resend = 0;
    int nc = 0;
    int sockbuf = 4 * 1024 * 1024;
    int smuxver = 2;
    int smuxbuf = 4 * 1024 * 1024;
    int framesize = 8192;
    int streambuf = 2 * 1024 * 1024;
    int keepalive = 10;
    int closewait = 0;
    std::string snmplog;
    int snmpperiod = 60;
    std::string log;
    std::string loglevel = "info";
    bool quiet = false;
    bool tcp = false;
    bool pprof = false;
    bool qpp = false;
    int qpp_count = 61;
};

struct ClientConfig : BaseConfig {
    std::string localaddr = ":12948";
    std::string remoteaddr = "vps:29900";
    int conn = 1;
    int autoexpire = 0;
    int scavengettl = 600;

    ClientConfig();
};

struct ServerConfig : BaseConfig {
    std::string listen = ":29900";
    std::string target = "127.0.0.1:12948";

    ServerConfig();
};

class HelpRequested final : public std::exception {
public:
    const char* what() const noexcept override { return "help requested"; }
};

class VersionRequested final : public std::exception {
public:
    const char* what() const noexcept override { return "version requested"; }
};

ClientConfig parse_client_config(int argc, char* argv[]);
ServerConfig parse_server_config(int argc, char* argv[]);

void apply_mode(BaseConfig& config);
void validate_common_config(const BaseConfig& config);
void log_effective_client_config(const ClientConfig& config);
void log_effective_server_config(const ServerConfig& config);
void log_compatibility_notes(const BaseConfig& config, bool client_mode);

std::string client_usage(std::string_view program);
std::string server_usage(std::string_view program);

inline constexpr std::string_view version = "SHIMAKAZE-SELFBUILD";

} // namespace shimakaze
