#pragma once

#include "shimakaze/utils.hpp"

#include <memory>

namespace shimakaze {

class DiagnosticsServerHandle {
public:
    virtual ~DiagnosticsServerHandle() = default;
};

std::shared_ptr<DiagnosticsServerHandle> start_diagnostics_server(asio::io_context& io, bool enabled);

} // namespace shimakaze
