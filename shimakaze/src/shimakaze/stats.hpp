#pragma once

#include "shimakaze/utils.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace shimakaze {

enum class SnmpField {
    bytes_sent,
    bytes_received,
    max_conn,
    active_opens,
    passive_opens,
    curr_estab,
    in_errs,
    in_csum_errors,
    kcp_in_errors,
    in_pkts,
    out_pkts,
    in_segs,
    out_segs,
    in_bytes,
    out_bytes,
    retrans_segs,
    fast_retrans_segs,
    early_retrans_segs,
    lost_segs,
    repeat_segs,
    fec_full_shard_set,
    fec_parity_shards,
    fec_errs,
    fec_recovered,
    fec_shard_set,
    fec_shard_min,
    ring_buffer_snd_queue,
    ring_buffer_rcv_queue,
    ring_buffer_snd_buffer,
    oob_packets,
};

void snmp_add(SnmpField field, std::uint64_t value = 1);
void snmp_store(SnmpField field, std::uint64_t value);
void snmp_connection_open(bool active);
void snmp_connection_close();

std::vector<std::string> snmp_header();
std::vector<std::string> snmp_values();
std::string snmp_json();

class SnmpLoggerHandle {
public:
    virtual ~SnmpLoggerHandle() = default;
};

std::shared_ptr<SnmpLoggerHandle> start_snmp_logger(asio::io_context& io, std::string path, int interval_seconds);

} // namespace shimakaze
