#pragma once

#include <sgrn/gateway/adapters/ethernetip/errors.hpp>
#include <sgrn/gateway/common/AdapterBase.hpp>
#include <sgrn/gateway/security/SecurityManager.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/wrappers/ethernetip/Server.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/types.hpp>

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace sgrn::gateway::adapters::ethernetip
{

class EipAdapter : public common::AdapterBase<EipAdapter> {
public:
    /// Typed, adapter-scoped error for the cascading sgrn::Result pattern —
    /// see <sgrn/gateway/adapters/ethernetip/errors.hpp>.

    explicit EipAdapter(twin::PlcMemory& t_memory, std::shared_ptr<::sgrn::gateway::SecurityManager> tsp_security_manager);
    ~EipAdapter();

    EipAdapter(const EipAdapter&) = delete;
    EipAdapter& operator=(const EipAdapter&) = delete;

    /**
     * @brief Bind to the interface and start the EtherNet/IP adapter.
     * Stores configuration and launches the serve loop.
     */
    sgrn::Result<void, ::sgrn::scl::SclError> start(const std::string& t_ip, uint16_t t_port, const ::sgrn::scl::PlcSchemaStore& t_store);

    void stop();

    // Callback handlers (must be public for C-style callbacks)
    int handlePreGet(void* tp_instance, void* tp_attribute, uint8_t t_service);
    int handlePostSet(void* tp_instance, void* tp_attribute, uint8_t t_service);

private:
    friend class common::AdapterBase<EipAdapter>;

    // AdapterBase calls these during lifecycle
    bool configure(const std::string& t_ip, uint16_t t_port);
    void serveLoop();

    std::optional<wrappers::ethernetip::Server> server_;
    std::string config_ip_;
    uint16_t config_port_{0};
    const ::sgrn::scl::PlcSchemaStore* p_store_{nullptr};

    struct AllocatedAttribute {
        uint16_t db_number;
        std::string field_path;
        ::sgrn::scl::DataType type;
        size_t size;
        int field_offset;
        int bit_index;
        s7codec::Endian endianness;
        ::sgrn::scl::DbField field; // Store the schema field tree
        void* buffer{nullptr};
        bool is_array{false};
    };

    std::vector<std::shared_ptr<AllocatedAttribute>> allocated_attrs_;
    std::map<std::pair<uint16_t, uint16_t>, std::shared_ptr<AllocatedAttribute>> attr_map_;
    /// Maps S7 DB number → the actual CIP instance number OpENer assigned
    std::map<uint16_t, uint16_t> db_to_cip_instance_;
};

} // namespace sgrn::gateway::adapters::ethernetip
