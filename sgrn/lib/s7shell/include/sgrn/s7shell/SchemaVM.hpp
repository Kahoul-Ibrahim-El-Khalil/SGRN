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
    /// For String/WString/XString/XWString scalar fields: character capacity N in STRING[N].
    /// Populated in registerFieldProperties from DbField::count (for scalars) or DbField::string_capacity.
    uint32_t string_capacity{0};
    s7codec::Endian endian{s7codec::Endian::Big};
};

/// Per-UDT-array metadata attached as auxiliary to UDT array getters.
struct UdtArrayMeta {
    std::string path;
    std::string udt_name;
    int count;
};

/// Value kind of a tag-table property: selects the AngelScript accessor
/// signature AND the JSON conversion. Scalar kinds are live, fully typed
/// properties (tags.x = v type-checks at compile time); Json covers UDT
/// tags (and anything without a scalar mapping) as a JSON document string.
enum class TagAsKind { Bool, Int, UInt, Int64, UInt64, Float, Double, String, Json };

/// Per-tag metadata attached as auxiliary to each registered TagTable accessor.
struct TagMeta {
    std::string tag_name; ///< runtime/file tag-table row name (unsanitized)
    TagAsKind kind{TagAsKind::String};
    /// AS return-slot width for Int/UInt kinds (1/2/4 bytes). The generic
    /// interface needs width-exact SetReturnByte/Word/DWord — a DWORD write
    /// into an int16 slot corrupts the call frame.
    uint32_t as_width{4};
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
    std::vector<std::unique_ptr<TagMeta>> tag_metas;
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

/// Register per-tag typed accessors on the TagTable type, so a
/// `TagTable@ tags = cli.tags();` handle exposes `tags.tag_name` with a
/// compile-time-checked type (bool/int/float/double/string, or a JSON
/// document string for UDT tags). Backed by the same routed get/put as the
/// dynamic tags.get()/put() API, so runtime, file-table, online and offline
/// tags all work through one surface.
void registerTagPropertyAccessors(
    sgrn::scripting::ScriptHost& t_host, const sgrn::scl::PlcSchemaStore& t_store, SchemaVMRegistry& t_registry = g_schema_registry);

/// Sanitize a field name to valid C identifier.
std::string sanitizeFieldName(const std::string& t_name);

} // namespace sgrn::s7shell::shell
