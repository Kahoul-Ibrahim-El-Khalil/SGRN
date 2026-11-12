#include <sgrn/gateway/twin/FieldTraversal.hpp>
#include <sgrn/gateway/twin/LeafDictionary.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>

#include <fmt/core.h>
#include <sgrn/utils/json-dom.hpp>
#include <charconv>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>

using sgrn::Result;
namespace sgrn::gateway::twin
{

LeafDictionary LeafDictionary::buildFrom(const scl::PlcSchemaStore& t_store) {
    LeafDictionary dict;
    LeafId next_id = 0;

    for (const auto& [db_num, schema] : t_store.dbs()) {
        // Match the naming convention PlcState::registerSegment() already uses
        // (see PlcState.cpp:27118) — dictionary paths must agree with the actual
        // top-level segment/JSON key names, or every lookup silently misses.
        const std::string db_prefix = schema.db_name.empty() ? fmt::format("DB{}", db_num) : schema.db_name;

        visitDbFields(schema.fields, [&](const DbFieldVisitInfo& t_info) {
            if (t_info.is_leaf) {
                const std::string full_path = db_prefix + "." + t_info.path;
                dict.path_by_id.push_back(full_path);
                dict.path_to_id[full_path] = next_id;
                ++next_id;
            }
        });
    }

    return dict;
}
Result<void, std::string> expandRecordKeys(const sgrn::json::dom::Value& t_record, const std::vector<std::string>& t_path_by_id,
    sgrn::json::dom::Document::AllocatorType& t_alloc, sgrn::json::dom::Value& t_out) {

    if (!t_record.is_object())
        return Error("Record is not a JSON object");

    t_out.setObject();

    // Keep a non-owning view into the input record. Copying payload_value with
    // Value's owning copy constructor duplicated the entire data/changes tree
    // before we copied its leaves into the output.
    sgrn::json::dom::Value payload_view;
    const sgrn::json::dom::Value* payload = nullptr;
    std::string_view payload_key;
    if (t_record.hasMember("data") && t_record["data"].is_object()) {
        payload_view.rebindTo(t_record["data"]);
        payload = &payload_view;
        payload_key = "data";
    } else if (t_record.hasMember("changes") && t_record["changes"].is_object()) {
        payload_view.rebindTo(t_record["changes"]);
        payload = &payload_view;
        payload_key = "changes";
    }

    for (auto it = t_record.memberBegin(); it != t_record.memberEnd(); ++it) {
        const std::string_view key{it->name.getString(), it->name.getStringLength()};
        if (payload && key == payload_key) {
            sgrn::json::dom::Value expanded_payload(sgrn::json::dom::Type::Object);
            for (auto p_it = payload->memberBegin(); p_it != payload->memberEnd(); ++p_it) {
                const std::string_view numeric_key{p_it->name.getString(), p_it->name.getStringLength()};
                LeafId id{};
                bool known_id = false;
                if (!numeric_key.empty()) {
                    const auto [end, error] = std::from_chars(numeric_key.data(), numeric_key.data() + numeric_key.size(), id);
                    known_id = error == std::errc{} && end == numeric_key.data() + numeric_key.size() && id < t_path_by_id.size();
                }
                if (known_id) {
                    expanded_payload.addMember(t_path_by_id[id], p_it->value, t_alloc);
                } else {
                    // Preserve unknown/non-numeric keys exactly as stored.
                    expanded_payload.addMember(numeric_key, p_it->value, t_alloc);
                }
            }
            t_out.addMember(payload_key, expanded_payload, t_alloc);
        } else {
            // AddMember performs the one required deep copy into t_out. The
            // old CopyFrom()+AddMember() sequence copied every subtree twice.
            t_out.addMember(key, it->value, t_alloc);
        }
    }

    return {};
}

// ── flattenNestedTree ────────────────────────────────────────────────────────
// Internal recursive helper that walks a nested JSON object and emits flat
// id-keyed entries for every leaf whose dotted path resolves in path_to_id.
namespace
{
void flattenWalk(const sgrn::json::dom::Value& t_node, std::string& t_prefix,
    const ankerl::unordered_dense::map<std::string, LeafId>& t_path_to_id, const std::vector<bool>* t_allowed,
    sgrn::json::dom::Document::AllocatorType& t_alloc, sgrn::json::dom::Value& t_out) {
    using JsonValue = sgrn::json::dom::Value;

    auto emitLeaf = [&](LeafId t_id, const JsonValue& t_value) {
        if (t_allowed && t_id < t_allowed->size() && !(*t_allowed)[t_id])
            return;

        char id_buffer[std::numeric_limits<LeafId>::digits10 + 2];
        const auto [id_end, id_error] = std::to_chars(std::begin(id_buffer), std::end(id_buffer), t_id);
        if (id_error != std::errc{})
            return;

        t_out.addMember(std::string_view{id_buffer, static_cast<size_t>(id_end - id_buffer)}, t_value, t_alloc);
    };

    if (t_node.is_object()) {
        for (auto it = t_node.memberBegin(); it != t_node.memberEnd(); ++it) {
            const size_t previous_size = t_prefix.size();
            if (!t_prefix.empty())
                t_prefix.push_back('.');
            t_prefix.append(it->name.getString(), it->name.getStringLength());
            flattenWalk(it->value, t_prefix, t_path_to_id, t_allowed, t_alloc, t_out);
            t_prefix.resize(previous_size);
        }
    } else if (t_node.is_array()) {
        // If this prefix is itself a dictionary leaf (array-typed field), emit
        // the whole array directly. Otherwise recurse into array-of-structs.
        const auto id_it = t_path_to_id.find(t_prefix);
        if (id_it != t_path_to_id.end()) {
            emitLeaf(id_it->second, t_node);
            return;
        }

        for (sgrn::json::dom::SizeType i = 0; i < t_node.size(); ++i) {
            const size_t previous_size = t_prefix.size();
            char index_buffer[std::numeric_limits<sgrn::json::dom::SizeType>::digits10 + 2];
            const auto [index_end, index_error] = std::to_chars(std::begin(index_buffer), std::end(index_buffer), i);
            if (index_error != std::errc{})
                continue;

            t_prefix.push_back('[');
            t_prefix.append(index_buffer, static_cast<size_t>(index_end - index_buffer));
            t_prefix.push_back(']');
            flattenWalk(t_node[i], t_prefix, t_path_to_id, t_allowed, t_alloc, t_out);
            t_prefix.resize(previous_size);
        }
    } else {
        const auto id_it = t_path_to_id.find(t_prefix);
        if (id_it != t_path_to_id.end())
            emitLeaf(id_it->second, t_node);
    }
}

} // anonymous namespace

Result<void, std::string> flattenNestedTree(const sgrn::json::dom::Value& t_nested,
    const ankerl::unordered_dense::map<std::string, LeafId>& t_path_to_id, sgrn::json::dom::Document::AllocatorType& t_alloc,
    sgrn::json::dom::Value& t_out) {
    RETURN_ERROR_IF(!t_nested.is_object(), "flattenNestedTree: input is not a JSON object");
    t_out.setObject();
    std::string prefix;
    prefix.reserve(128);
    flattenWalk(t_nested, prefix, t_path_to_id, nullptr, t_alloc, t_out);
    return {};
}

Result<void, std::string> flattenNestedTreeFiltered(const sgrn::json::dom::Value& t_nested,
    const ankerl::unordered_dense::map<std::string, LeafId>& t_path_to_id, const std::vector<bool>& t_allowed,
    sgrn::json::dom::Document::AllocatorType& t_alloc, sgrn::json::dom::Value& t_out) {

    RETURN_ERROR_IF(!t_nested.is_object(), "flattenNestedTreeFiltered: input is not a JSON object");

    t_out.setObject();
    std::string prefix;
    prefix.reserve(128);
    flattenWalk(t_nested, prefix, t_path_to_id, &t_allowed, t_alloc, t_out);
    return {};
}

} // namespace sgrn::gateway::twin
