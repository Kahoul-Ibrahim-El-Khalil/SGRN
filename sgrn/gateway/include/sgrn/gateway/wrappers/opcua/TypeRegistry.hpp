#pragma once

#include <sgrn/Result.hpp>

#include <open62541/types.h>

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct UA_ServerConfig;

namespace sgrn::gateway::wrappers::opcua
{

/// Description of an SCL-derived OPC UA Enumeration data type.
struct EnumTypeDef {
    std::string name;                  ///< UA DataType name e.g. "MotorState".
    std::string signature;             ///< Stable identity (== keys used in `enum_by_signature_`).
    std::map<int, std::string> values; ///< Ordered value → symbolic-name map.
};

/// Open62541 UDT registration payload produced by the adapter layer.
struct UdtRegistrationBatch {
    std::vector<std::string> names;
    std::vector<std::vector<UA_DataTypeMember>> members;
    std::vector<UA_DataType> types;
    std::unordered_map<std::string, const UA_DataType*> index_;

    /// First-class OPC UA Enumeration data types derived from SCL `#ENUM` aliases
    /// and inline `#ENUM` field attributes. Each entry mirrors a `UA_DataType`
    /// already present in `types` (typeKind == UA_DATATYPEKIND_ENUM); this vector
    /// keeps the value→name map so address-space registration can build the
    /// EnumStrings/EnumValues properties.
    std::vector<EnumTypeDef> enums;
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
    const UA_DataType* findEnumBySignature(std::string_view t_signature) const;

    const std::vector<UA_DataType>& types() const noexcept {
        return custom_types_;
    }

    /// Enumeration metadata for every `UA_DATATYPEKIND_ENUM` entry in `types()`.
    const std::vector<EnumTypeDef>& enumDefinitions() const noexcept {
        return enum_defs_;
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

    std::vector<EnumTypeDef> enum_defs_;
    std::unordered_map<std::string, const UA_DataType*> enum_by_signature_;
};

} // namespace sgrn::gateway::wrappers::opcua
