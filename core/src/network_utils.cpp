#include <string>
#include <vector>
#include <glog/logging.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <algorithm>

namespace ifex {
namespace network {

std::vector<std::string> get_local_ip_addresses() {
    std::vector<std::string> addresses;
    struct ifaddrs* ifaddr = nullptr;
    
    if (getifaddrs(&ifaddr) == -1) {
        LOG(ERROR) << "Failed to get network interfaces: " << strerror(errno);
        return addresses;
    }
    
    // Iterate through all interfaces
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        
        // Only interested in IPv4 addresses
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        
        // Skip loopback interfaces
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        
        // Skip interfaces that are not up
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        
        // Get the IP address
        char host[INET_ADDRSTRLEN];
        struct sockaddr_in* addr = (struct sockaddr_in*)ifa->ifa_addr;
        if (inet_ntop(AF_INET, &addr->sin_addr, host, INET_ADDRSTRLEN) != nullptr) {
            addresses.push_back(std::string(host));
            LOG(INFO) << "Found local IP: " << host << " on interface " << ifa->ifa_name;
        }
    }
    
    freeifaddrs(ifaddr);
    
    // Remove duplicates
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
    
    return addresses;
}

std::string get_primary_ip_address() {
    // Simple heuristic: connect to a well-known address and see which local IP is used
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        LOG(ERROR) << "Failed to create socket: " << strerror(errno);
        return "";
    }
    
    // Use Google's DNS as a well-known address (we don't actually send data)
    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_port = htons(53);
    serv.sin_addr.s_addr = inet_addr("8.8.8.8");
    
    if (connect(sock, (struct sockaddr*)&serv, sizeof(serv)) < 0) {
        LOG(ERROR) << "Failed to connect socket: " << strerror(errno);
        close(sock);
        return "";
    }
    
    struct sockaddr_in name;
    socklen_t namelen = sizeof(name);
    if (getsockname(sock, (struct sockaddr*)&name, &namelen) < 0) {
        LOG(ERROR) << "Failed to get socket name: " << strerror(errno);
        close(sock);
        return "";
    }
    
    char buffer[INET_ADDRSTRLEN];
    const char* result = inet_ntop(AF_INET, &name.sin_addr, buffer, INET_ADDRSTRLEN);
    close(sock);
    
    if (result) {
        LOG(INFO) << "Primary IP address: " << buffer;
        return std::string(buffer);
    }
    
    return "";
}

// Additional utility functions
std::vector<std::string> resolve_bind_address(const std::string& bind_address) {
    std::vector<std::string> resolved_addresses;
    
    auto parse_endpoint = [](const std::string& endpoint) -> std::pair<std::string, uint16_t> {
        size_t colon_pos = endpoint.find_last_of(':');
        if (colon_pos == std::string::npos) {
            return {endpoint, 0};
        }
        
        std::string host = endpoint.substr(0, colon_pos);
        std::string port_str = endpoint.substr(colon_pos + 1);
        
        uint16_t port = 0;
        try {
            port = static_cast<uint16_t>(std::stoi(port_str));
        } catch (...) {
            LOG(ERROR) << "Invalid port in endpoint: " << endpoint;
        }
        
        return {host, port};
    };
    
    auto is_any_address = [](const std::string& address) -> bool {
        return address == "0.0.0.0" || address == "::" || address.empty();
    };
    
    auto [host, port] = parse_endpoint(bind_address);
    
    if (is_any_address(host)) {
        // Get all local IPs and append the port
        auto local_ips = get_local_ip_addresses();
        
        // If no specific IPs found, try primary IP
        if (local_ips.empty()) {
            std::string primary = get_primary_ip_address();
            if (!primary.empty()) {
                local_ips.push_back(primary);
            }
        }
        
        // If still no IPs, fall back to localhost (not ideal but better than nothing)
        if (local_ips.empty()) {
            LOG(WARNING) << "No local IPs found, falling back to localhost";
            local_ips.push_back("127.0.0.1");
        }
        
        for (const auto& ip : local_ips) {
            resolved_addresses.push_back(ip + ":" + std::to_string(port));
        }
    } else {
        // Not a bind-all address, use as-is
        resolved_addresses.push_back(bind_address);
    }
    
    return resolved_addresses;
}

} // namespace network
} // namespace ifex