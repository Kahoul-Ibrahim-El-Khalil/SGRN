#pragma once

#include <sgrn/Result.hpp>

#include <open62541/types.h>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct UA_ServerConfig;

namespace sgrn::gateway::wrappers::opcua
{

/// Open62541 UDT registration payload produced by the adapter layer.
struct UdtRegistrationBatch {
    std::vector<std::string> names;
    std::vector<std::vector<UA_DataTypeMember>> members;
    std::vector<UA_DataType> types;
    std::unordered_map<std::string, const UA_DataType*> index_;
};

/// Owns custom UA_DataType descriptors for an OPC-UA server.
///
/// Population happens in the adapter (PlcSchemaStore → UdtRegistrationBatch).
/// This class only stores open62541 registration data and installs it into
/// UA_ServerConfig.
class TypeRegistry {
public:
    TypeRegistry() = default;
    ~TypeRegistry() = default;

    TypeRegistry(TypeRegistry&&) = default;
    TypeRegistry& operator=(TypeRegistry&&) = default;
    TypeRegistry(const TypeRegistry&) = delete;
    TypeRegistry& operator=(const TypeRegistry&) = delete;

    void clear();

    /// Take ownership of a batch built by the adapter translation layer.
    sgrn::Result<void> adopt(UdtRegistrationBatch&& t_batch);

    /// Chain into config->customDataTypes (matches legacy registerUdtTypes behaviour).
    void attachTo(UA_ServerConfig* tp_config);

    const UA_DataType* find(std::string_view t_udt_name) const;

    const std::vector<UA_DataType>& types() const noexcept {
        return custom_types_;
    }

    const UA_DataTypeArray* dataTypeArray() const noexcept {
        return custom_data_types_array_.get();
    }

private:
    std::vector<std::string> udt_names_;
    std::vector<std::vector<UA_DataTypeMember>> custom_members_;
    std::vector<UA_DataType> custom_types_;
    std::unique_ptr<UA_DataTypeArray> custom_data_types_array_;
    std::unordered_map<std::string, const UA_DataType*> index_;
};

} // namespace sgrn::gateway::wrappers::opcua
