#include "shimakaze/config.hpp"

#include "shimakaze/logger.hpp"
#include "shimakaze/qpp.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace shimakaze {
namespace {

std::string normalize_key(std::string_view key)
{
    std::string out;
    out.reserve(key.size());
    for (const char ch : key) {
        if (ch != '-' && ch != '_') {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    return out;
}

bool is_bool_key(std::string_view key)
{
    static const std::vector<std::string> keys {
        "nocomp", "acknodelay", "quiet", "tcp", "pprof", "qpp",
    };
    const auto normalized = normalize_key(key);
    return std::ranges::find(keys, normalized) != keys.end();
}

bool parse_bool(std::string_view value)
{
    auto normalized = normalize_key(value);
    if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
        return false;
    }
    throw std::runtime_error("invalid boolean value: " + std::string(value));
}

int parse_int(std::string_view key, std::string_view value)
{
    try {
        size_t used = 0;
        const auto result = std::stoi(std::string(value), &used, 10);
        if (used != value.size()) {
            throw std::runtime_error("");
        }
        return result;
    } catch (...) {
        throw std::runtime_error("invalid integer for " + std::string(key) + ": " + std::string(value));
    }
}

std::string read_file(const std::string& path)
{
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("failed to open config file: " + path);
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

struct CliScan {
    std::unordered_map<std::string, std::string> values;
    std::string config_path;
};

std::pair<std::string, std::optional<std::string>> split_argument(std::string_view arg)
{
    while (arg.starts_with('-')) {
        arg.remove_prefix(1);
    }
    const auto eq = arg.find('=');
    if (eq == std::string_view::npos) {
        return {std::string(arg), std::nullopt};
    }
    return {std::string(arg.substr(0, eq)), std::string(arg.substr(eq + 1))};
}

CliScan scan_args(int argc, char* argv[])
{
    CliScan scan;
    for (int i = 1; i < argc; ++i) {
        std::string_view raw = argv[i];
        if (raw == "-h" || raw == "--help" || raw == "help") {
            throw HelpRequested {};
        }
        if (raw == "-v" || raw == "--version" || raw == "version") {
            throw VersionRequested {};
        }
        if (!raw.starts_with('-')) {
            throw std::runtime_error("unexpected positional argument: " + std::string(raw));
        }

        auto [key, inline_value] = split_argument(raw);
        auto normalized = normalize_key(key);
        std::string value;
        if (inline_value) {
            value = *inline_value;
        } else if (is_bool_key(normalized)) {
            value = "true";
        } else {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for flag: " + key);
            }
            value = argv[++i];
        }

        if (normalized == "c" || normalized == "config") {
            scan.config_path = value;
        } else {
            scan.values[normalized] = value;
        }
    }
    return scan;
}

void assign_common(BaseConfig& config, std::string_view raw_key, std::string_view value)
{
    const auto key = normalize_key(raw_key);
    if (key == "key") config.key = std::string(value);
    else if (key == "crypt") config.crypt = std::string(value);
    else if (key == "mode") config.mode = std::string(value);
    else if (key == "mtu") config.mtu = parse_int(raw_key, value);
    else if (key == "ratelimit") config.ratelimit = parse_int(raw_key, value);
    else if (key == "sndwnd") config.sndwnd = parse_int(raw_key, value);
    else if (key == "rcvwnd") config.rcvwnd = parse_int(raw_key, value);
    else if (key == "datashard" || key == "ds") config.datashard = parse_int(raw_key, value);
    else if (key == "parityshard" || key == "ps") config.parityshard = parse_int(raw_key, value);
    else if (key == "dscp") config.dscp = parse_int(raw_key, value);
    else if (key == "nocomp") config.nocomp = parse_bool(value);
    else if (key == "acknodelay") config.acknodelay = parse_bool(value);
    else if (key == "nodelay") config.nodelay = parse_int(raw_key, value);
    else if (key == "interval") config.interval = parse_int(raw_key, value);
    else if (key == "resend") config.resend = parse_int(raw_key, value);
    else if (key == "nc") config.nc = parse_int(raw_key, value);
    else if (key == "sockbuf") config.sockbuf = parse_int(raw_key, value);
    else if (key == "smuxver") config.smuxver = parse_int(raw_key, value);
    else if (key == "smuxbuf") config.smuxbuf = parse_int(raw_key, value);
    else if (key == "framesize") config.framesize = parse_int(raw_key, value);
    else if (key == "streambuf") config.streambuf = parse_int(raw_key, value);
    else if (key == "keepalive") config.keepalive = parse_int(raw_key, value);
    else if (key == "closewait") config.closewait = parse_int(raw_key, value);
    else if (key == "snmplog") config.snmplog = std::string(value);
    else if (key == "snmpperiod") config.snmpperiod = parse_int(raw_key, value);
    else if (key == "log") config.log = std::string(value);
    else if (key == "loglevel") config.loglevel = std::string(value);
    else if (key == "quiet") config.quiet = parse_bool(value);
    else if (key == "tcp") config.tcp = parse_bool(value);
    else if (key == "pprof") config.pprof = parse_bool(value);
    else if (key == "qpp") config.qpp = parse_bool(value);
    else if (key == "qppcount") config.qpp_count = parse_int(raw_key, value);
    else throw std::runtime_error("unknown common option: " + std::string(raw_key));
}

std::string json_value_to_string(const boost::json::value& value)
{
    if (value.is_string()) {
        return std::string(value.as_string());
    }
    if (value.is_bool()) {
        return value.as_bool() ? "true" : "false";
    }
    if (value.is_int64()) {
        return std::to_string(value.as_int64());
    }
    if (value.is_uint64()) {
        return std::to_string(value.as_uint64());
    }
    throw std::runtime_error("JSON config values must be string, bool, or integer");
}

template <typename Config>
void apply_json_common(Config& config, const boost::json::object& object)
{
    for (const auto& [key, value] : object) {
        const std::string_view key_view(key.data(), key.size());
        const auto normalized = normalize_key(key_view);
        if (normalized == "localaddr" || normalized == "remoteaddr" || normalized == "conn" ||
            normalized == "autoexpire" || normalized == "scavengettl" ||
            normalized == "listen" || normalized == "target") {
            continue;
        }
        assign_common(config, key_view, json_value_to_string(value));
    }
}

void load_json(ClientConfig& config, const std::string& path)
{
    const auto value = boost::json::parse(read_file(path));
    const auto& object = value.as_object();
    apply_json_common(config, object);

    if (auto* v = object.if_contains("localaddr")) config.localaddr = json_value_to_string(*v);
    if (auto* v = object.if_contains("remoteaddr")) config.remoteaddr = json_value_to_string(*v);
    if (auto* v = object.if_contains("conn")) config.conn = parse_int("conn", json_value_to_string(*v));
    if (auto* v = object.if_contains("autoexpire")) config.autoexpire = parse_int("autoexpire", json_value_to_string(*v));
    if (auto* v = object.if_contains("scavengettl")) config.scavengettl = parse_int("scavengettl", json_value_to_string(*v));
}

void load_json(ServerConfig& config, const std::string& path)
{
    const auto value = boost::json::parse(read_file(path));
    const auto& object = value.as_object();
    apply_json_common(config, object);

    if (auto* v = object.if_contains("listen")) config.listen = json_value_to_string(*v);
    if (auto* v = object.if_contains("target")) config.target = json_value_to_string(*v);
}

template <typename Config>
void apply_cli_common(Config& config, const CliScan& scan)
{
    for (const auto& [key, value] : scan.values) {
        const auto normalized = normalize_key(key);
        if (normalized == "localaddr" || normalized == "l" || normalized == "remoteaddr" ||
            normalized == "r" || normalized == "conn" || normalized == "autoexpire" ||
            normalized == "scavengettl" || normalized == "listen" || normalized == "target" ||
            normalized == "t") {
            continue;
        }
        assign_common(config, key, value);
    }
}

void apply_client_cli(ClientConfig& config, const CliScan& scan)
{
    apply_cli_common(config, scan);
    for (const auto& [key, value] : scan.values) {
        const auto normalized = normalize_key(key);
        if (normalized == "localaddr" || normalized == "l") config.localaddr = value;
        else if (normalized == "remoteaddr" || normalized == "r") config.remoteaddr = value;
        else if (normalized == "conn") config.conn = parse_int(key, value);
        else if (normalized == "autoexpire") config.autoexpire = parse_int(key, value);
        else if (normalized == "scavengettl") config.scavengettl = parse_int(key, value);
    }
}

void apply_server_cli(ServerConfig& config, const CliScan& scan)
{
    apply_cli_common(config, scan);
    for (const auto& [key, value] : scan.values) {
        const auto normalized = normalize_key(key);
        if (normalized == "listen" || normalized == "l") config.listen = value;
        else if (normalized == "target" || normalized == "t") config.target = value;
    }
}

void apply_env(BaseConfig& config)
{
    if (const char* env = std::getenv("SHIMAKAZE_KEY"); env != nullptr && *env != '\0') {
        config.key = env;
    }
}

void sanitize_common(BaseConfig& config)
{
    if (config.ratelimit < 0) {
        config.ratelimit = 0;
    }
}

void log_common(const BaseConfig& config)
{
    auto& log = Logger::instance();
    log.info("encryption: ", config.crypt);
    log.info("QPP: ", config.qpp);
    log.info("QPP Count: ", config.qpp_count);
    log.info("nodelay parameters: ", config.nodelay, " ", config.interval, " ", config.resend, " ", config.nc);
    log.info("sndwnd: ", config.sndwnd, " rcvwnd: ", config.rcvwnd);
    log.info("compression: ", !config.nocomp);
    log.info("mtu: ", config.mtu);
    log.info("ratelimit: ", config.ratelimit);
    log.info("datashard: ", config.datashard, " parityshard: ", config.parityshard);
    log.info("acknodelay: ", config.acknodelay);
    log.info("dscp: ", config.dscp);
    log.info("sockbuf: ", config.sockbuf);
    log.info("smuxver: ", config.smuxver);
    log.info("smuxbuf: ", config.smuxbuf);
    log.info("framesize: ", config.framesize);
    log.info("streambuf: ", config.streambuf);
    log.info("keepalive: ", config.keepalive);
    log.info("closewait: ", config.closewait);
    log.info("snmplog: ", config.snmplog);
    log.info("snmpperiod: ", config.snmpperiod);
    log.info("loglevel: ", config.loglevel);
    log.info("quiet: ", config.quiet);
    log.info("tcp: ", config.tcp);
    log.info("pprof: ", config.pprof);
}

} // namespace

ClientConfig::ClientConfig()
{
    sndwnd = 128;
    rcvwnd = 512;
    closewait = 0;
}

ServerConfig::ServerConfig()
{
    sndwnd = 1024;
    rcvwnd = 1024;
    closewait = 30;
}

ClientConfig parse_client_config(int argc, char* argv[])
{
    ClientConfig config;
    apply_env(config);
    const auto scan = scan_args(argc, argv);
    apply_client_cli(config, scan);
    if (!scan.config_path.empty()) {
        load_json(config, scan.config_path);
    }
    apply_mode(config);
    sanitize_common(config);
    validate_common_config(config);
    if (config.conn <= 0) {
        throw std::runtime_error("conn must be greater than 0");
    }
    return config;
}

ServerConfig parse_server_config(int argc, char* argv[])
{
    ServerConfig config;
    apply_env(config);
    const auto scan = scan_args(argc, argv);
    apply_server_cli(config, scan);
    if (!scan.config_path.empty()) {
        load_json(config, scan.config_path);
    }
    apply_mode(config);
    sanitize_common(config);
    validate_common_config(config);
    return config;
}

void apply_mode(BaseConfig& config)
{
    const auto mode = normalize_key(config.mode);
    if (mode == "normal") {
        config.nodelay = 0;
        config.interval = 40;
        config.resend = 2;
        config.nc = 1;
    } else if (mode == "fast") {
        config.nodelay = 0;
        config.interval = 30;
        config.resend = 2;
        config.nc = 1;
    } else if (mode == "fast2") {
        config.nodelay = 1;
        config.interval = 20;
        config.resend = 2;
        config.nc = 1;
    } else if (mode == "fast3") {
        config.nodelay = 1;
        config.interval = 10;
        config.resend = 2;
        config.nc = 1;
    }
}

void validate_common_config(const BaseConfig& config)
{
    if (config.mtu < 576 || config.mtu > 65535) {
        throw std::runtime_error("mtu must be between 576 and 65535");
    }
    if (config.interval <= 0) {
        throw std::runtime_error("interval must be greater than 0");
    }
    if (config.sndwnd <= 0 || config.rcvwnd <= 0) {
        throw std::runtime_error("sndwnd and rcvwnd must be greater than 0");
    }
    if (config.datashard < 0 || config.parityshard < 0 || config.datashard + config.parityshard > 256) {
        throw std::runtime_error("datashard and parityshard must be non-negative and sum to at most 256");
    }
    if (config.smuxver > 2) {
        throw std::runtime_error("unsupported smux version: " + std::to_string(config.smuxver));
    }
    if (config.smuxbuf <= 0 || config.framesize <= 0 || config.streambuf <= 0 || config.keepalive <= 0) {
        throw std::runtime_error("smuxbuf, framesize, streambuf, and keepalive must be greater than 0");
    }
    if (config.qpp && (config.qpp_count <= 0 || config.qpp_count > 65535)) {
        throw std::runtime_error("QPPCount must be in range 1..65535 when QPP is enabled");
    }
    Logger::instance().set_level(config.loglevel);
}

void log_effective_client_config(const ClientConfig& config)
{
    auto& log = Logger::instance();
    log.info("version: ", version);
    log.info("listening on: ", config.localaddr);
    log.info("remote address: ", config.remoteaddr);
    log_common(config);
    log.info("conn: ", config.conn);
    log.info("autoexpire: ", config.autoexpire);
    log.info("scavengettl: ", config.scavengettl);
}

void log_effective_server_config(const ServerConfig& config)
{
    auto& log = Logger::instance();
    log.info("version: ", version);
    log.info("listening on: ", config.listen);
    log.info("target: ", config.target);
    log_common(config);
}

void log_compatibility_notes(const BaseConfig& config, bool client_mode)
{
    auto& log = Logger::instance();
    if (config.qpp) {
        for (const auto& warning : validate_qpp_params(config.qpp_count, config.key)) {
            log.warn(warning);
        }
    }
    if (config.tcp) {
        log.warn("--tcp raw transport is parsed but not implemented; UDP transport will be used.");
    }
    if (client_mode) {
        const auto& client = static_cast<const ClientConfig&>(config);
        if (client.autoexpire != 0 && client.scavengettl > client.autoexpire) {
            log.warn("scavengettl is bigger than autoexpire; connections may race hard to use bandwidth.");
        }
    }
}

std::string client_usage(std::string_view program)
{
    std::ostringstream os;
    os << "Usage: " << program << " [options]\n\n"
       << "Shimakaze client options:\n"
       << "  -l, --localaddr <addr>      local listen address (default :12948)\n"
       << "  -r, --remoteaddr <addr>     KCP server address or port range (default vps:29900)\n"
       << "  --key <secret>              pre-shared secret, also SHIMAKAZE_KEY\n"
       << "  --crypt <name>              null, none, aes, aes-128/192, salsa20, sm4, tea, xor...\n"
       << "  --mode <profile>            fast3, fast2, fast, normal, manual\n"
       << "  --mtu <bytes>               UDP MTU (default 1350)\n"
       << "  --sndwnd <n> --rcvwnd <n>   KCP windows\n"
       << "  --nocomp                    disable compression flag\n"
       << "  --quiet                     reduce stream logs\n"
       << "  --loglevel <level>          trace, debug, info, warn, error, off\n"
       << "  -c <file>                   JSON config overriding command flags\n"
       << "  -h, --help                  show help\n";
    return os.str();
}

std::string server_usage(std::string_view program)
{
    std::ostringstream os;
    os << "Usage: " << program << " [options]\n\n"
       << "Shimakaze server options:\n"
       << "  -l, --listen <addr>         KCP listen address or port range (default :29900)\n"
       << "  -t, --target <addr>         TCP target address (default 127.0.0.1:12948)\n"
       << "  --key <secret>              pre-shared secret, also SHIMAKAZE_KEY\n"
       << "  --crypt <name>              null, none, aes, aes-128/192, salsa20, sm4, tea, xor...\n"
       << "  --mode <profile>            fast3, fast2, fast, normal, manual\n"
       << "  --mtu <bytes>               UDP MTU (default 1350)\n"
       << "  --sndwnd <n> --rcvwnd <n>   KCP windows\n"
       << "  --nocomp                    disable compression flag\n"
       << "  --quiet                     reduce stream logs\n"
       << "  --loglevel <level>          trace, debug, info, warn, error, off\n"
       << "  -c <file>                   JSON config overriding command flags\n"
       << "  -h, --help                  show help\n";
    return os.str();
}

} // namespace shimakaze
