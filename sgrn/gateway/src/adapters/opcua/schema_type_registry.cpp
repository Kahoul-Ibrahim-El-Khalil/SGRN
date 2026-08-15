#include <sgrn/gateway/adapters/opcua/TypeTranslation.hpp>
#include <sgrn/gateway/adapters/opcua/schema_type_registry.hpp>
#include <sgrn/gateway/wrappers/opcua/TypeRegistry.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/types.hpp>

#include <functional>
#include <open62541/types_generated.h>
#include <unordered_map>

using ::sgrn::scl::DataType;
using ::sgrn::scl::DbField;

namespace sgrn::gateway::adapters
{

sgrn::Result<void> populateTypeRegistryFromSchema(
    const ::sgrn::scl::PlcSchemaStore& t_registry, wrappers::opcua::TypeRegistry& t_type_registry) {
    // Legacy registerUdtTypes() from main — schema translation lives in the adapter.
    const auto& udts = t_registry.udts();

    std::unordered_map<std::string, const ::sgrn::scl::UdtDefinition*> udt_index;
    udt_index.reserve(udts.size());
    for (const auto& p_udt : udts)
        udt_index[p_udt.name] = &p_udt;

    std::vector<const ::sgrn::scl::UdtDefinition*> sorted;
    std::unordered_map<std::string, bool> visited;
    std::function<void(const ::sgrn::scl::UdtDefinition&)> dfs = [&](const ::sgrn::scl::UdtDefinition& p_udt) {
        if (visited.count(p_udt.name))
            return;
        visited[p_udt.name] = false;
        for (const auto& f : p_udt.fields) {
            if (f.type == DataType::Struct && !f.udt_name.empty()) {
                auto it = udt_index.find(f.udt_name);
                if (it != udt_index.end())
                    dfs(*it->second);
            }
        }
        visited[p_udt.name] = true;
        sorted.push_back(&p_udt);
    };
    for (const auto& p_udt : udts)
        dfs(p_udt);

    wrappers::opcua::UdtRegistrationBatch batch;
    const size_t udt_count = sorted.size();
    batch.names.reserve(udt_count);
    batch.members.reserve(udt_count);
    batch.types.reserve(udt_count);
    batch.index_.reserve(udt_count);

    std::unordered_map<std::string, const UA_DataType*> building_index;

    uint32_t udt_idx = 0;
    for (const ::sgrn::scl::UdtDefinition* p_udt : sorted) {
        UA_DataType ut{};
        std::vector<UA_DataTypeMember> members;
        batch.names.push_back(p_udt->name);
        ut.typeName = batch.names.back().c_str();

        ut.typeId = UA_NODEID_NUMERIC(1, 10000 + udt_idx);
        ut.binaryEncodingId = UA_NODEID_NUMERIC(1, 20000 + udt_idx);
        ++udt_idx;

        ut.typeKind = UA_DATATYPEKIND_STRUCTURE;
        ut.pointerFree = true;
        size_t current_offset = 0;
        for (const DbField& f : p_udt->fields) {
            UA_DataTypeMember m{};
#ifdef UA_ENABLE_TYPEDESCRIPTION
            m.memberName = f.name.c_str();
#endif
            const UA_DataType* mt = nullptr;
            if (f.type == DataType::Struct && !f.udt_name.empty()) {
                auto it = building_index.find(f.udt_name);
                mt = (it != building_index.end()) ? it->second : &UA_TYPES[UA_TYPES_BYTE];
            } else {
                int idx = s7TypeToUaTypeIndex(f.type);
                mt = (idx >= 0) ? &UA_TYPES[idx] : &UA_TYPES[UA_TYPES_BYTE];
            }
            if (mt->typeKind == UA_DATATYPEKIND_STRING || f.count > 1 || !mt->pointerFree)
                ut.pointerFree = false;

            size_t align = mt->memSize > 8 ? 8 : (mt->memSize == 0 ? 1 : mt->memSize);
            size_t padding = (current_offset % align == 0) ? 0 : align - (current_offset % align);
            m.memberType = mt;
            m.padding = static_cast<UA_Byte>(padding);
            m.isArray = (f.count > 1);
            current_offset += padding;
            if (m.isArray)
                current_offset += sizeof(size_t) + sizeof(void*);
            else
                current_offset += mt->memSize;
            members.push_back(m);
        }
        if (current_offset % 8 != 0)
            current_offset += 8 - (current_offset % 8);

        ut.memSize = static_cast<UA_UInt32>(current_offset);
        ut.membersSize = static_cast<UA_UInt32>(members.size());
        batch.members.push_back(std::move(members));
        ut.members = batch.members.back().data();
        batch.types.push_back(ut);
        building_index[p_udt->name] = &batch.types.back();
    }

    for (size_t i = 0; i < batch.types.size(); ++i)
        batch.types[i].members = batch.members[i].data();

    batch.index_.clear();
    for (size_t i = 0; i < sorted.size(); ++i)
        batch.index_[sorted[i]->name] = &batch.types[i];

    for (size_t i = 0; i < batch.types.size(); ++i) {
        UA_DataType& ut = batch.types[i];
        const ::sgrn::scl::UdtDefinition* p_udt = sorted[i];
        for (size_t j = 0; j < ut.membersSize; ++j) {
            UA_DataTypeMember& m = const_cast<UA_DataTypeMember*>(ut.members)[j];
            const DbField& f = p_udt->fields[j];
            if (f.type == DataType::Struct && !f.udt_name.empty()) {
                auto it = batch.index_.find(f.udt_name);
                if (it != batch.index_.end())
                    m.memberType = it->second;
            }
        }
    }

    return t_type_registry.adopt(std::move(batch));
}

} // namespace sgrn::gateway::adapters
