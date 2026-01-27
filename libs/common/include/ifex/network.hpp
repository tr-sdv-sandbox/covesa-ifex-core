#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <utility>

namespace ifex {
namespace network {

// Get all non-loopback IPv4 addresses for this machine
std::vector<std::string> get_local_ip_addresses();

// Get the primary (default route) IP address
std::string get_primary_ip_address();

// Convert 0.0.0.0:port to actual IP:port endpoints
std::vector<std::string> resolve_bind_address(const std::string& bind_address);

} // namespace network
} // namespace ifex