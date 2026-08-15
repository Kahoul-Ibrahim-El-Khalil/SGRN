#include <sgrn/utils/network.hpp>

#include <arpa/inet.h>
#include <asio.hpp>

#include <algorithm>
#include <cstring>

namespace sgrn::utils::network
{

// ---- Internal helpers ----

namespace
{

// Get peer address from socket.
NetworkResult<sockaddr_storage> getPeerAddress(int fd) noexcept {
    sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        return NetworkIpParsingError::GetPeerNameFailed;
    return addr;
}

// Get local address from socket.
NetworkResult<sockaddr_storage> getSockName(int fd) noexcept {
    sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        return NetworkIpParsingError::GetSockNameFailed;
    return addr;
}

// Convert sockaddr_storage to IPv4/IPv6 string; optionally return port.
NetworkResult<std::string> addressToString(const sockaddr_storage& addr, uint16_t* out_port = nullptr) {
    char ip[INET6_ADDRSTRLEN];
    uint16_t port = 0;

    if (addr.ss_family == AF_INET) {
        auto* sin = reinterpret_cast<const sockaddr_in*>(&addr);
        if (!::inet_ntop(AF_INET, &sin->sin_addr, ip, INET_ADDRSTRLEN))
            return NetworkIpParsingError::IpConversionFailed;
        port = ntohs(sin->sin_port);
    } else if (addr.ss_family == AF_INET6) {
        auto* sin6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        if (!::inet_ntop(AF_INET6, &sin6->sin6_addr, ip, INET6_ADDRSTRLEN))
            return NetworkIpParsingError::IpConversionFailed;
        port = ntohs(sin6->sin6_port);
    } else {
        return NetworkIpParsingError::AddressFamilyUnsupported;
    }

    if (out_port)
        *out_port = port;
    return std::string(ip);
}

} // namespace

// ---- Error strings ----

const char* toString(NetworkIpParsingError code) noexcept {
    using enum NetworkIpParsingError;
    switch (code) {
        case IpStringTooLong:
            return "IP string too long";
        case InetPtonFailed:
            return "inet_pton failed – invalid IPv4";
        case ResolveFailed:
            return "DNS resolution failed";
        case InvalidCidrFormat:
            return "CIDR missing '/'";
        case InvalidPrefixLength:
            return "prefix length must be 0‑32";
        case GetPeerNameFailed:
            return "getpeername() failed";
        case GetSockNameFailed:
            return "getsockname() failed";
        case IpConversionFailed:
            return "inet_ntop() failed";
        case AddressFamilyUnsupported:
            return "unsupported address family";
        default:
            return "unknown error";
    }
}

// ---- Public implementations ----

NetworkResult<uint32_t> parseIpv4Strict(std::string_view t_str) noexcept {
    char buf[64];
    if (t_str.size() >= sizeof(buf))
        return NetworkIpParsingError::IpStringTooLong;

    std::copy(t_str.begin(), t_str.end(), buf);
    buf[t_str.size()] = '\0';

    in_addr addr{};
    if (::inet_pton(AF_INET, buf, &addr) == 1)
        return ntohl(addr.s_addr);

    return NetworkIpParsingError::InetPtonFailed;
}

NetworkResult<uint32_t> parseIpv4OrResolve(const std::string& t_str) {
    // Strict parse first
    auto strict = parseIpv4Strict(t_str);
    if (!strict.hasError())
        return strict.value();

    // DNS fallback
    try {
        asio::io_context io;
        asio::ip::tcp::resolver resolver(io);
        auto results = resolver.resolve(t_str, "");
        if (!results.empty()) {
            std::string ip = results.begin()->endpoint().address().to_string();
            in_addr addr{};
            if (::inet_pton(AF_INET, ip.c_str(), &addr) == 1)
                return ntohl(addr.s_addr);
        }
    } catch (const asio::system_error&) {
        // fall through
    } catch (...) {
        // fall through
    }
    return NetworkIpParsingError::ResolveFailed;
}

NetworkResult<CidrMatcher> parseIp(const std::string& t_ip) {
    auto host = parseIpv4OrResolve(t_ip);
    if (host.hasError())
        return NetworkResult<CidrMatcher>::Error(host.error());
    return CidrMatcher{host.value(), 0xFFFFFFFF};
}

NetworkResult<CidrMatcher> parseCidr(const std::string& t_cidr) {
    auto slash = t_cidr.find('/');
    if (slash == std::string::npos)
        return parseIp(t_cidr);

    std::string ip_part = t_cidr.substr(0, slash);
    std::string len_part = t_cidr.substr(slash + 1);

    auto host = parseIpv4OrResolve(ip_part);
    if (host.hasError())
        return NetworkResult<CidrMatcher>::Error(host.error());

    int prefix_len = 0;
    try {
        prefix_len = std::stoi(len_part);
    } catch (const std::invalid_argument&) {
        return NetworkIpParsingError::InvalidPrefixLength;
    } catch (const std::out_of_range&) {
        return NetworkIpParsingError::InvalidPrefixLength;
    }

    if (prefix_len < 0 || prefix_len > 32)
        return NetworkIpParsingError::InvalidPrefixLength;

    uint32_t mask = (prefix_len == 0) ? 0 : (~0u << (32 - prefix_len));
    return CidrMatcher{host.value() & mask, mask};
}

NetworkResult<std::string> getClientIp(int t_fd) {
    auto addr = getPeerAddress(t_fd);
    if (addr.hasError())
        return NetworkResult<std::string>::Error(addr.error());
    return addressToString(addr.value());
}

NetworkResult<uint16_t> getClientPort(int t_fd) {
    auto addr = getPeerAddress(t_fd);
    if (addr.hasError())
        return NetworkResult<uint16_t>::Error(addr.error());
    uint16_t port = 0;
    auto str = addressToString(addr.value(), &port);
    if (str.hasError())
        return NetworkResult<uint16_t>::Error(str.error());
    return port;
}

NetworkResult<std::string> getLocalAddress(int t_fd) {
    auto addr = getSockName(t_fd);
    if (addr.hasError())
        return NetworkResult<std::string>::Error(addr.error());
    uint16_t port = 0;
    auto str = addressToString(addr.value(), &port);
    if (str.hasError())
        return NetworkResult<std::string>::Error(str.error());
    return str.value() + ":" + std::to_string(port);
}

} // namespace sgrn::utils::network
