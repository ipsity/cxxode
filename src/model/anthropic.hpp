#pragma once

#include "core/session.hpp"

namespace lay {

// Reads ANTHROPIC_API_KEY in the transport. The destination is fixed.
Generate anthropic_model(std::string model, StopRequested stop_requested);

namespace messages {
Generate loopback_model(std::string model, std::uint16_t port, StopRequested stop_requested);
}

} // namespace lay
