#include <sgrn/gateway/wrappers/opcua/TypeRegistry.hpp>

#include <open62541/server.h>

namespace sgrn::gateway::wrappers::opcua
{

void TypeRegistry::clear() {
    udt_names_.clear();
    custom_members_.clear();
    custom_types_.clear();
    custom_data_types_array_.reset();
    index_.clear();
    enum_defs_.clear();
    enum_by_signature_.clear();
}

sgrn::Result<void> TypeRegistry::adopt(UdtRegistrationBatch&& t_batch) {
    clear();
    udt_names_ = std::move(t_batch.names);
    custom_members_ = std::move(t_batch.members);
    custom_types_ = std::move(t_batch.types);
    index_ = std::move(t_batch.index_);
    enum_defs_ = std::move(t_batch.enums);

    for (size_t i = 0; i < custom_types_.size(); ++i)
        custom_types_[i].members = custom_members_[i].data();

    enum_by_signature_.clear();
    for (const auto& enum_def : enum_defs_) {
        auto it = index_.find(enum_def.name);
        if (it != index_.end()) {
            enum_by_signature_[enum_def.signature] = it->second;
        }
    }

    if (!custom_types_.empty()) {
        custom_data_types_array_ =
            std::make_unique<UA_DataTypeArray>(UA_DataTypeArray{nullptr, custom_types_.size(), custom_types_.data()});
    }
    return {};
}

void TypeRegistry::attachTo(UA_ServerConfig* tp_config) {
    if (!tp_config)
        return;

    if (custom_types_.empty()) {
        custom_data_types_array_.reset();
        return;
    }

    custom_data_types_array_ =
        std::make_unique<UA_DataTypeArray>(UA_DataTypeArray{tp_config->customDataTypes, custom_types_.size(), custom_types_.data()});
    tp_config->customDataTypes = custom_data_types_array_.get();
}

const UA_DataType* TypeRegistry::find(std::string_view t_udt_name) const {
    auto it = index_.find(std::string(t_udt_name));
    return it != index_.end() ? it->second : nullptr;
}

const UA_DataType* TypeRegistry::findEnumBySignature(std::string_view t_signature) const {
    auto it = enum_by_signature_.find(std::string(t_signature));
    return it != enum_by_signature_.end() ? it->second : nullptr;
}

} // namespace sgrn::gateway::wrappers::opcua
