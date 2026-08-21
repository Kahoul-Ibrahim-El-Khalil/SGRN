#include <sgrn/gateway/adapters/opcua/TypeTranslation.hpp>
#include <sgrn/gateway/adapters/opcua/schema_type_registry.hpp>
#include <sgrn/gateway/wrappers/opcua/TypeRegistry.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/types.hpp>
#include <sgrn/scl/utils.hpp>

#include <fmt/core.h>
#include <functional>
#include <open62541/types_generated.h>
#include <unordered_map>

using ::sgrn::scl::DataType;
using ::sgrn::scl::DbField;

namespace sgrn::gateway::adapters
{

// ─── Helper: collect enum definitions from schema ───────────────────────────
static std::unordered_map<std::string, wrappers::opcua::EnumTypeDef> collectEnums(const ::sgrn::scl::PlcSchemaStore& t_registry) {
    std::unordered_map<std::string, wrappers::opcua::EnumTypeDef> enum_by_sig;

    auto collectEnum = [&](const DbField& f, const std::string& t_name) {
        if (f.enum_map.empty())
            return;
        const int ua_base = s7TypeToUaTypeIndex(f.type);
        const std::string sig = enumTypeSignature(ua_base, f.enum_map);
        auto [it, inserted] = enum_by_sig.emplace(sig, wrappers::opcua::EnumTypeDef{});
        if (inserted) {
            it->second.name = t_name;
            it->second.signature = sig;
            it->second.values = f.enum_map;
        } else if (it->second.name.empty()) {
            it->second.name = t_name;
        }
    };

    // Scalar‑alias UDTs with #ENUM
    for (const auto& p_udt : t_registry.udts()) {
        if (p_udt.is_scalar_alias && !p_udt.enum_map.empty()) {
            DbField field{};
            field.name = p_udt.name;
            field.type = p_udt.scalar_type;
            field.enum_map = p_udt.enum_map;
            collectEnum(field, p_udt.name);
        }
    }

    // Inline #ENUM attributes in UDTs and DataBlocks
    for (const auto& p_udt : t_registry.udts()) {
        for (const auto& f : p_udt.fields)
            collectEnum(f, p_udt.name);
    }
    for (const auto& p_db : t_registry.dbs()) {
        ::sgrn::scl::forEachField(
            p_db.second.fields, "", 0, [&](const DbField& f, const std::string&, int) { collectEnum(f, p_db.second.db_name); });
    }

    return enum_by_sig;
}

// ─── Helper: build sorted list of structure UDTs (topological order) ────────
static std::vector<const ::sgrn::scl::UdtDefinition*> sortUdtDependencies(
    const std::unordered_map<std::string, const ::sgrn::scl::UdtDefinition*>& udt_index) {
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

    for (const auto& p_udt : udt_index) {
        if (!p_udt.second->is_scalar_alias)
            dfs(*p_udt.second);
    }
    return sorted;
}

// ─── Main function ───────────────────────────────────────────────────────────
sgrn::Result<void> populateTypeRegistryFromSchema(
    const ::sgrn::scl::PlcSchemaStore& t_registry, wrappers::opcua::TypeRegistry& t_type_registry) {

    // Build index of non‑scalar UDTs
    const auto& udts = t_registry.udts();
    std::unordered_map<std::string, const ::sgrn::scl::UdtDefinition*> udt_index;
    udt_index.reserve(udts.size());
    for (const auto& p_udt : udts) {
        if (!p_udt.is_scalar_alias)
            udt_index[p_udt.name] = &p_udt;
    }

    // Sorted structure UDTs (dependencies first)
    auto sorted = sortUdtDependencies(udt_index);

    // Collect enum definitions
    auto enum_by_sig = collectEnums(t_registry);
    const size_t enum_count = enum_by_sig.size();
    const size_t udt_count = sorted.size();

    wrappers::opcua::UdtRegistrationBatch batch;
    batch.names.reserve(udt_count + enum_count);
    batch.members.reserve(udt_count + enum_count);
    batch.types.reserve(udt_count + enum_count);
    batch.index_.reserve(udt_count + enum_count);
    batch.enums.reserve(enum_count);

    std::unordered_map<std::string, const UA_DataType*> building_index;
    std::unordered_map<std::string, const UA_DataType*> enum_index_by_sig;

    // ── 1. Build enumeration types ──────────────────────────────────────────
    uint32_t enum_idx = 0;
    for (auto& [sig, def] : enum_by_sig) {
        if (def.name.empty())
            def.name = fmt::format("S7Enum_{}", enum_idx);
        UA_DataType et{};
        et.typeId = UA_NODEID_NUMERIC(1, 30000 + enum_idx);
        et.binaryEncodingId = UA_NODEID_NUMERIC(1, 0);
        ++enum_idx;
        et.typeKind = UA_DATATYPEKIND_ENUM;
        et.pointerFree = true;
        et.overlayable = UA_BINARY_OVERLAYABLE_INTEGER;
        et.memSize = static_cast<UA_UInt32>(sizeof(UA_Int32));
        et.members = nullptr;
        et.membersSize = 0;
        batch.names.push_back(def.name);
        et.typeName = batch.names.back().c_str();
        batch.members.push_back({});
        batch.types.push_back(et);
        building_index[def.name] = &batch.types.back();
        enum_index_by_sig[sig] = &batch.types.back();
        batch.index_[def.name] = &batch.types.back();
        batch.enums.push_back(std::move(def));
    }

    // ── 2. Build structure types ────────────────────────────────────────────
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
            if (!f.enum_map.empty()) {
                const int ua_base = s7TypeToUaTypeIndex(f.type);
                const std::string sig = enumTypeSignature(ua_base, f.enum_map);
                auto enum_it = enum_index_by_sig.find(sig);
                if (enum_it != enum_index_by_sig.end())
                    mt = enum_it->second;
            }
            if (!mt && f.type == DataType::Struct && !f.udt_name.empty()) {
                auto it = building_index.find(f.udt_name);
                mt = (it != building_index.end()) ? it->second : &UA_TYPES[UA_TYPES_BYTE];
            }
            if (!mt) {
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

    // Ensure all members pointers are set
    for (size_t i = 0; i < batch.types.size(); ++i)
        batch.types[i].members = batch.members[i].data();

    // ── 3. Re‑build index for structures only (enums already in index) ──────
    for (size_t i = 0; i < sorted.size(); ++i) {
        // Structures start at offset `enum_count` in batch.types
        batch.index_[sorted[i]->name] = &batch.types[enum_count + i];
    }

    // ── 4. Fix‑up member type pointers for nested UDTs ──────────────────────
    for (size_t i = 0; i < sorted.size(); ++i) {
        UA_DataType& ut = batch.types[enum_count + i]; // structure type
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
