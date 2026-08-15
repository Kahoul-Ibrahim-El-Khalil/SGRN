#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// SchemaVM.hpp  –  Schema-driven typed virtual machine for AngelScript
//
// Dynamically registers PLC data block types from schema metadata.
// Each DB becomes an AS ref type with native property accessors that
// encode/decode directly to/from the raw S7 buffer via s7codec.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <s7codec/codec.hpp>

#include <sgrn/scripting/ScriptHost.hpp>

#include <sgrn/scl/schema/PlcSchemaStore.hpp>

namespace sgrn::s7shell::shell
{

/// Per-field metadata attached as auxiliary to each registered AS accessor.
struct FieldMeta {
    std::string path;
    s7codec::Type s7type;
    int abs_offset{0};
    int bit_index{0};
    int count{0};
    s7codec::Endian endian{s7codec::Endian::Big};
};

/// Per-UDT-array metadata attached as auxiliary to UDT array getters.
struct UdtArrayMeta {
    std::string path;
    std::string udt_name;
    int count;
};

/// All per-engine registration state.
/// One instance must be owned per asIScriptEngine lifetime.
/// Storing these as globals caused use-after-free when a second engine
/// was created after the first was destroyed — the dangling FieldMeta/
/// UdtArrayMeta pointers remained registered as AS auxiliary data.
struct SchemaVMRegistry {
    std::vector<std::unique_ptr<FieldMeta>> field_meta;
    std::vector<std::unique_ptr<UdtArrayMeta>> udt_array_metas;
    std::vector<std::unique_ptr<std::string>> udt_field_names;
    std::unordered_set<std::string> registered_schema_types;
    std::unordered_set<std::string> registered_udt_properties;
};

/// Cached engine pointer — set once by registerS7Shell().
/// This remains a global because it is a plain pointer, not heap metadata
/// with auxiliary lifetime dependencies.
extern asIScriptEngine* p_g_as_engine;

/// Per-engine registry instance. One shell = one engine = one registry.
extern SchemaVMRegistry g_schema_registry;

struct ScriptS7Connection;

/// Current connection scope used by generated DB factories such as Plant().
/// Creating an S7Client, or attaching one to a PlcRuntime, updates this.
extern ScriptS7Connection* p_g_active_connection;

/// Register all DB types from a PlcSchemaStore as AngelScript ref types.
/// Called automatically by loadSchema()/loadSclSchema()/loadJsonSchema().
void registerSchemaTypes(
    sgrn::scripting::ScriptHost& t_host, const sgrn::scl::PlcSchemaStore& t_store, SchemaVMRegistry& t_registry = g_schema_registry);

/// Register DataBlock property accessors on PlcRuntime and S7Client types.
/// After calling this, `rt.DbName` and `plc.DbName` return ScriptDataBlock@
/// handles for each DB in the schema store.
void registerDbPropertyAccessors(
    sgrn::scripting::ScriptHost& t_host, const sgrn::scl::PlcSchemaStore& t_store, SchemaVMRegistry& t_registry = g_schema_registry);

/// Sanitize a field name to valid C identifier.
std::string sanitizeFieldName(const std::string& t_name);

} // namespace sgrn::s7shell::shell
