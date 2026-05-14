#include "shimakaze/stats.hpp"

#include "shimakaze/logger.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <stdexcept>
#include <utility>

namespace shimakaze {
namespace {

constexpr std::array<std::string_view, 30> field_names {
    "BytesSent",
    "BytesReceived",
    "MaxConn",
    "ActiveOpens",
    "PassiveOpens",
    "CurrEstab",
    "InErrs",
    "InCsumErrors",
    "KCPInErrors",
    "InPkts",
    "OutPkts",
    "InSegs",
    "OutSegs",
    "InBytes",
    "OutBytes",
    "RetransSegs",
    "FastRetransSegs",
    "EarlyRetransSegs",
    "LostSegs",
    "RepeatSegs",
    "FECFullShards",
    "FECParityShards",
    "FECErrs",
    "FECRecovered",
    "FECShardSet",
    "FECShardMin",
    "RingBufferSndQueue",
    "RingBufferRcvQueue",
    "RingBufferSndBuffer",
    "OOBPackets",
};

constexpr std::size_t index_of(SnmpField field)
{
    return static_cast<std::size_t>(field);
}

struct SnmpStorage {
    std::array<std::atomic_uint64_t, field_names.size()> counters {};
};

SnmpStorage& storage()
{
    static SnmpStorage value;
    return value;
}

void replace_all(std::string& text, std::string_view from, std::string_view to)
{
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string go_time_path_to_strftime(std::string path)
{
    replace_all(path, "2006", "%Y");
    replace_all(path, "06", "%y");
    replace_all(path, "01", "%m");
    replace_all(path, "02", "%d");
    replace_all(path, "15", "%H");
    replace_all(path, "04", "%M");
    replace_all(path, "05", "%S");
    return path;
}

std::string format_time_path(const std::string& path)
{
    const auto fmt = go_time_path_to_strftime(path);
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    std::tm tm {};
#if defined(_WIN32)
    localtime_s(&tm, &seconds);
#else
    localtime_r(&seconds, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, fmt.c_str());
    return os.str();
}

std::string unix_seconds()
{
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    return std::to_string(seconds);
}

void write_csv_record(const std::string& path)
{
    const auto formatted_path = format_time_path(path);
    const auto file_path = std::filesystem::path(formatted_path);
    const auto write_header = !std::filesystem::exists(file_path) || std::filesystem::file_size(file_path) == 0;

    std::ofstream file(file_path, std::ios::app);
    if (!file) {
        throw std::runtime_error("failed to open snmp log: " + formatted_path);
    }

    if (write_header) {
        file << "Unix";
        for (const auto& field : field_names) {
            file << ',' << field;
        }
        file << '\n';
    }

    file << unix_seconds();
    for (const auto& value : snmp_values()) {
        file << ',' << value;
    }
    file << '\n';
}

class SnmpLogger final : public SnmpLoggerHandle, public std::enable_shared_from_this<SnmpLogger> {
public:
    SnmpLogger(asio::io_context& io, std::string path, int interval_seconds)
        : timer_(io)
        , path_(std::move(path))
        , interval_(std::chrono::seconds(interval_seconds))
    {
    }

    void start()
    {
        schedule();
    }

private:
    void schedule()
    {
        timer_.expires_after(interval_);
        auto self = shared_from_this();
        timer_.async_wait([self](const boost::system::error_code& error) {
            if (error) {
                return;
            }
            try {
                write_csv_record(self->path_);
            } catch (const std::exception& ex) {
                Logger::instance().warn("snmp logger: ", ex.what());
            }
            self->schedule();
        });
    }

    asio::steady_timer timer_;
    std::string path_;
    std::chrono::seconds interval_;
};

} // namespace

void snmp_add(SnmpField field, std::uint64_t value)
{
    storage().counters[index_of(field)].fetch_add(value, std::memory_order_relaxed);
}

void snmp_store(SnmpField field, std::uint64_t value)
{
    storage().counters[index_of(field)].store(value, std::memory_order_relaxed);
}

void snmp_connection_open(bool active)
{
    snmp_add(active ? SnmpField::active_opens : SnmpField::passive_opens);
    const auto current = storage().counters[index_of(SnmpField::curr_estab)].fetch_add(1, std::memory_order_relaxed) + 1;
    auto observed = storage().counters[index_of(SnmpField::max_conn)].load(std::memory_order_relaxed);
    while (current > observed &&
           !storage().counters[index_of(SnmpField::max_conn)].compare_exchange_weak(observed,
                                                                                     current,
                                                                                     std::memory_order_relaxed)) {
    }
}

void snmp_connection_close()
{
    auto& counter = storage().counters[index_of(SnmpField::curr_estab)];
    auto current = counter.load(std::memory_order_relaxed);
    while (current > 0 &&
           !counter.compare_exchange_weak(current, current - 1, std::memory_order_relaxed)) {
    }
}

std::vector<std::string> snmp_header()
{
    std::vector<std::string> out;
    out.reserve(field_names.size());
    for (const auto& field : field_names) {
        out.emplace_back(field);
    }
    return out;
}

std::vector<std::string> snmp_values()
{
    std::vector<std::string> out;
    out.reserve(field_names.size());
    for (const auto& counter : storage().counters) {
        out.push_back(std::to_string(counter.load(std::memory_order_relaxed)));
    }
    return out;
}

std::string snmp_json()
{
    std::ostringstream os;
    os << "{";
    const auto values = snmp_values();
    for (std::size_t i = 0; i < field_names.size(); ++i) {
        if (i != 0) {
            os << ",";
        }
        os << "\"" << field_names[i] << "\":" << values[i];
    }
    os << "}";
    return os.str();
}

std::shared_ptr<SnmpLoggerHandle> start_snmp_logger(asio::io_context& io, std::string path, int interval_seconds)
{
    if (path.empty() || interval_seconds <= 0) {
        return {};
    }
    auto logger = std::make_shared<SnmpLogger>(io, std::move(path), interval_seconds);
    logger->start();
    return logger;
}

} // namespace shimakaze
