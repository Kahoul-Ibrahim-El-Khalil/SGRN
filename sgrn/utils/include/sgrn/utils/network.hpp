#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <sgrn/Result.hpp> // provides sgrn::Result

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

namespace sgrn::utils::network
{

// Error codes for IP parsing and socket operations
enum class NetworkIpParsingError {
    IpStringTooLong,
    InetPtonFailed,
    ResolveFailed,
    InvalidCidrFormat,
    InvalidPrefixLength,
    GetPeerNameFailed,
    GetSockNameFailed,
    AddressFamilyUnsupported,
    IpConversionFailed,
};

// Convert error code to a human‑readable string.
const char* toString(NetworkIpParsingError code) noexcept;

template <typename T>
using NetworkResult = sgrn::Result<T, NetworkIpParsingError>;

// Holds a network address and mask for CIDR matching.
struct CidrMatcher {
    uint32_t network{0}; // host byte order
    uint32_t mask{0};

    bool match(uint32_t ip_hbo) const {
        return (ip_hbo & mask) == network;
    }
};

// ---- Parsing ----

// Parse CIDR string ("192.168.1.0/24") or plain IP (as /32). May resolve hostnames.
NetworkResult<CidrMatcher> parseCidr(const std::string& t_cidr);

// Parse plain IP (no DNS) as /32.
NetworkResult<CidrMatcher> parseIp(const std::string& t_ip);

// Strict IPv4 parsing (no DNS), returns host‑byte‑order uint32_t.
NetworkResult<uint32_t> parseIpv4Strict(std::string_view t_s) noexcept;

// Parse IPv4 or resolve hostname (blocking DNS), returns host‑byte‑order uint32_t.
NetworkResult<uint32_t> parseIpv4OrResolve(const std::string& t_s);

// ---- Socket helpers ----

// Get peer IP address of a connected socket.
NetworkResult<std::string> getClientIp(int t_fd);

// Get peer port of a connected socket.
NetworkResult<uint16_t> getClientPort(int t_fd);

// Get local address and port as "IP:port".
NetworkResult<std::string> getLocalAddress(int t_fd);

// Simple IP‑to‑pattern matching (exact, prefix, or wildcard "*").
inline bool ipMatches(const std::string& ip, const std::string& pattern) {
    if (pattern == "*")
        return true;
    if (pattern == ip)
        return true;
    if (ip.find(pattern) == 0)
        return true; // prefix match
    return false;
}

} // namespace sgrn::utils::network
