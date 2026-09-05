#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <sgrn/gateway/core/TelemetryBroker.hpp>
#include <sgrn/gateway/gateway.hpp>
#include <sgrn/gateway/twin/DbMemorySpan.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/scl/DbSchema.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <rapidjson/document.h>

namespace sgrn::gateway
{

struct ReplayConfig {
    std::string gateway_config_path;
    std::string archive_path;
    std::string scl_schema_path;
    double replay_speed = 1.0;
    bool loop = false;
    bool no_delay = false;
};

class GatewayReplayer {
public:
    explicit GatewayReplayer(const ReplayConfig& config);
    ~GatewayReplayer();

    sgrn::Result<void, std::string> initialize();
    void start();
    void stop();
    bool isRunning() const {
        return running_.load();
    }
    /// Configured HTTP (dashboard) port, or 0 when the HTTP adapter is off.
    uint16_t httpPort() const;

private:
    void replayLoop();
    bool processBinaryArchive(const std::string& path);

    /// Encode one archived leaf (dotted "DbName.field.path" + JSON scalar)
    /// into raw twin bytes. Appends the span to t_spans (backed by t_storage,
    /// which must outlive the spans). Single-bit bools are NOT encoded here:
    /// they share packed bytes with neighbours, so they are staged into
    /// pending_bits_ for writeBit() instead (same split as DbIOProvider).
    /// Returns false on schema drift or encode failure (caller logs + skips).
    bool encodeLeaf(const std::string& full_path, const rapidjson::Value& val, std::deque<std::vector<uint8_t>>& storage,
        std::vector<sgrn::gateway::twin::DbMemorySpan>& spans);

    /// Commits bits staged by encodeLeaf() via PlcMemory::writeBit()
    /// (read-modify-write per bit, so co-packed neighbours survive).
    /// Clears the staging list. Returns false if any write failed.
    bool flushPendingBits();

    ReplayConfig config_;
    GatewayConfig gateway_config_;
    /// Non-owning view of GatewayApplication's twin memory, populated in
    /// initialize() after initTwin(). Replay writes decoded frames straight
    /// into it via PlcMemory::writeDbMemory().
    PlcMemory* plc_memory_{nullptr};
    /// Non-owning view of GatewayApplication's compiled schema, populated in
    /// initialize() right after plc_memory_. Same lifetime as gateway_app_.
    const sgrn::scl::PlcSchemaStore* schema_store_{nullptr};
    /// Single-bit bools staged by encodeLeaf() for writeBit() commit.
    /// Cleared per WAL line by flushPendingBits().
    struct PendingBit {
        uint16_t db;
        size_t byte_offset;
        int bit_index;
        bool value;
    };
    std::vector<PendingBit> pending_bits_;
    std::shared_ptr<TelemetryBroker> broker_;
    std::unique_ptr<GatewayApplication> gateway_app_;

    std::atomic<bool> running_{false};
    std::thread replay_thread_;
};

} // namespace sgrn::gateway
