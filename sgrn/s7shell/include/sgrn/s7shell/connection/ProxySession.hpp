#pragma once
// =============================================================================
// ProxySession — s7shell embedded proxy session
//
// Mirrors the s7proxy standalone app behaviour directly inside s7shell.
// A single ProxySession manages one or many DB mappings from a source S7Client
// to a hub S7Client, performing polling + change-detection + write.
//
// Usage from AngelScript:
//   S7Client@ src = S7Client("192.168.1.10", 0, 1);
//   S7Client@ hub = S7Client("192.168.1.1", 0, 1);
//   S7ProxySession@ proxy = S7ProxySession(src, hub);
//   proxy.addMapping(1, 1, 100);  // srcDB=1, dstDB=1, 100ms interval
//   proxy.start();
//   // ... do other work ...
//   proxy.stop();
// =============================================================================

#include <sgrn/s7shell/connection/S7Connection.hpp>
#include <asio.hpp>
#include <asio/thread_pool.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace sgrn::s7shell::connection
{

struct ProxyMapping {
    uint16_t src_db{0};
    uint16_t dst_db{0};
    int interval_ms{100};
    size_t size_bytes{0}; ///< 0 means "resolve from src client's schema on first poll"

    // Wire-transfer scratch buffer only. The change-detection baseline
    // (formerly a private `last_state` buffer here, duplicating the dirty
    // tracking ScriptDataBlock already does) now lives in the src client's
    // PlcRuntime — see PlcRuntime::dbSnapshots()/markDirty()/takeDirty() —
    // so proxy mirroring and script-driven DB access share one dirty-region
    // mechanism instead of two independent ones.
    std::vector<uint8_t> buffer;
    bool first_run{true};
    std::shared_ptr<asio::steady_timer> timer;
};
using ProxyMappingSPtr = std::shared_ptr<ProxyMapping>;

/// Manages the async polling loop; lives entirely in C++ land.
class ProxySession : public std::enable_shared_from_this<ProxySession> {
public:
    ProxySession(shell::ScriptS7Client* tp_src, shell::ScriptS7Client* tp_hub);
    ~ProxySession();

    /// Add a DB mapping. Must be called before start().
    /// size_bytes: number of raw bytes to copy. Pass 0 to let the session
    /// read the whole DB using the schema size stored in the src client.
    void addMapping(uint16_t t_src_db, uint16_t t_dst_db, int t_interval_ms, uint32_t t_size_bytes = 0);

    void start();
    void stop();

    bool isRunning() const {
        return running_.load();
    }

private:
    void tick(ProxyMappingSPtr tsp_map);
    void execute(ProxyMappingSPtr tsp_map);
    void do_poll_and_push(ProxyMappingSPtr tsp_map);

    // These are raw pointers to AngelScript ref-counted objects.
    // The wrapper (ProxySessionWrapper, in bind_proxy.cpp) holds addRef and
    // calls release on destruction — ProxySession itself does NOT manage AS
    // ref-counting, it just borrows the objects through the wrapper's lifetime.
    shell::ScriptS7Client* src_client_;
    shell::ScriptS7Client* hub_client_;

    std::vector<ProxyMappingSPtr> mappings_;

    asio::io_context io_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_guard_;
    std::unique_ptr<asio::thread_pool> pool_;
    std::thread io_thread_;

    std::mutex src_mutex_;
    std::atomic<bool> running_{false};
};

} // namespace sgrn::s7shell::connection
