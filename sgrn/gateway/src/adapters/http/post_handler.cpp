#include <fmt/core.h>
#include <sgrn/gateway/adapters/http.hpp>
#include <sgrn/gateway/adapters/http/path.hpp>
#include <sgrn/gateway/twin/PlcCommandProcessor.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/utils/time.hpp>

namespace sgrn::gateway::adapters
{

void HttpAdapter::handlePost(
    const httplib::Request& t_req, httplib::Response& t_res, const PlcSchemaStore& t_registry, PlcMemory& t_memory) {
    // ── 1. Resolve URL path → DB schema + relative field path ──────────────
    std::string url_path = t_req.matches[1];
    if (!url_path.empty() && url_path.back() == '/')
        url_path.pop_back();

    if (url_path.empty()) {
        // (multi-DB POST path — unchanged below)
        // Multi-DB POST: The body must be a JSON object where keys are DB names.
        rapidjson::Document doc;
        doc.Parse(t_req.body.c_str());
        if (doc.HasParseError() || !doc.IsObject()) {
            t_res.status = 400;
            t_res.set_content(R"({"error":"POST /data/ requires a JSON object with DB names as keys"})", "application/json");
            return;
        }

        struct PendingLeaf {
            uint16_t db;
            std::string path;
            std::string value;
        };
        std::vector<PendingLeaf> all_leaves;

        const std::string& client_ip = t_req.remote_addr;

        for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
            std::string db_name = it->name.GetString();

            auto r = t_registry.getDbByName(db_name);
            if (r.hasError() || !r.value()) {
                t_res.status = 404;
                t_res.set_content(fmt::format(R"({{"error":"Unknown DB '{}'"}})", db_name), "application/json");
                return;
            }
            uint16_t db_num = r.value()->db_number;

            std::vector<std::pair<std::string, std::string>> db_leaves;
            collectLeaves(it->value, "", db_leaves);
            for (const auto& leaf : db_leaves) {
                if (!isAuthorizedField(t_req, db_num, leaf.first, true)) {
                    t_res.status = 403;
                    t_res.set_content(fmt::format(R"({{"error":"Forbidden: IP {} is not authorised to write path '{}' in DB{}"}})",
                                          client_ip, leaf.first, db_num),
                        "application/json");
                    return;
                }
                all_leaves.push_back({db_num, leaf.first, leaf.second});
            }
        }

        if (all_leaves.empty()) {
            t_res.status = 400;
            t_res.set_content(R"({"error":"JSON body produced no writable leaves"})", "application/json");
            return;
        }

        const uint64_t ts = static_cast<uint64_t>(sgrn::utils::time::nowMilliseconds());

        int written = 0;
        for (const auto& leaf : all_leaves) {
            auto r = t_memory.updateFieldWithTimestamp(leaf.db, leaf.path, leaf.value, ts);
            if (!r.hasError())
                ++written;
        }

        if (written == 0) {
            t_res.status = 422;
            t_res.set_content(R"({"error":"No fields were written — verify the paths and value types"})", "application/json");
            return;
        }

        t_memory.processor()->processCommands();

        t_res.status = 200;
        t_res.set_content(fmt::format(R"({{"fields_written":{}}})", written), "application/json");
        return;
    }

    auto [schema, field_path, array_index] = resolveSemanticPath(sgrn::utils::strings::tokenize(url_path, '/'), t_registry);

    if (!schema) {
        t_res.status = 404;
        t_res.set_content(fmt::format(R"({{"error":"Path '{}' does not resolve to a known DB"}})", url_path), "application/json");
        return;
    }

    const uint16_t db_num = schema->db_number;

    // ── 3. Parse JSON body ────────────────────────────────────────────────
    rapidjson::Document doc;
    doc.Parse(t_req.body.c_str());
    if (doc.HasParseError()) {
        t_res.status = 400;
        t_res.set_content(R"({"error":"Body is not valid JSON"})", "application/json");
        return;
    }

    // ── 3b. Indexed array element write (read-modify-write) ───────────────
    // POST /data/<DB>/<field>/<N>  with a scalar body → replace element N.
    if (array_index.has_value()) {
        if (doc.IsObject()) {
            t_res.status = 400;
            t_res.set_content(
                R"({"error":"Indexed array write expects a scalar or non-object value, not a JSON object"})", "application/json");
            return;
        }
        if (field_path.empty()) {
            t_res.status = 400;
            t_res.set_content(R"({"error":"Indexed array write requires a non-empty field path"})", "application/json");
            return;
        }

        if (!isAuthorizedField(t_req, db_num, field_path, true)) {
            t_res.status = 403;
            t_res.set_content(fmt::format(R"({{"error":"Forbidden: Not authorised to write path '{}'"}})", field_path), "application/json");
            return;
        }

        // 1. Read the current array
        auto arr_res = t_memory.getFieldValue(db_num, field_path);
        if (arr_res.hasError()) {
            t_res.status = 404;
            t_res.set_content(fmt::format(R"({{"error":"Array field '{}' not found"}})", field_path), "application/json");
            return;
        }

        rapidjson::Document arr_doc;
        if (arr_doc.Parse(arr_res.value().c_str()).HasParseError() || !arr_doc.IsArray()) {
            t_res.status = 422;
            t_res.set_content(fmt::format(R"({{"error":"Field '{}' is not a JSON array"}})", field_path), "application/json");
            return;
        }

        const size_t idx = *array_index;
        if (idx >= arr_doc.GetArray().Size()) {
            t_res.status = 416;
            t_res.set_content(fmt::format(R"X({{"error":"Array index {} out of range (size={})"}})X", idx, arr_doc.GetArray().Size()),
                "application/json");
            return;
        }

        // 2. Replace element at idx
        arr_doc[static_cast<rapidjson::SizeType>(idx)].CopyFrom(doc, arr_doc.GetAllocator());

        // 3. Serialize the modified array and write it back atomically
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        arr_doc.Accept(w);
        const std::string new_arr_json = sb.GetString();

        const uint64_t ts = static_cast<uint64_t>(sgrn::utils::time::nowMilliseconds());
        auto write_res = t_memory.updateFieldWithTimestamp(db_num, field_path, new_arr_json, ts);
        if (write_res.hasError()) {
            t_res.status = 422;
            t_res.set_content(fmt::format(R"({{"error":"Failed to write array field '{}'"}})", field_path), "application/json");
            return;
        }
        t_memory.processor()->processCommands();

        // 4. Serialize the updated element for the response
        rapidjson::StringBuffer elem_sb;
        rapidjson::Writer<rapidjson::StringBuffer> elem_w(elem_sb);
        arr_doc[static_cast<rapidjson::SizeType>(idx)].Accept(elem_w);

        t_res.status = 200;
        t_res.set_content(fmt::format(R"({{"db":{},"path":"{}","index":{},"value":{}}})", db_num, url_path, idx, elem_sb.GetString()),
            "application/json");
        return;
    }

    // ── 4. Collect leaf writes ────────────────────────────────────────────
    // If the body is a plain scalar (writing directly to a leaf field), treat
    // field_path as the single target and the entire body as its value.
    std::vector<std::pair<std::string, std::string>> leaves;

    if (!doc.IsObject()) {
        // Scalar or array body → write directly to the resolved field path
        if (field_path.empty()) {
            t_res.status = 400;
            t_res.set_content(R"({"error":"Scalar body requires a path pointing to a leaf field"})", "application/json");
            return;
        }
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        doc.Accept(w);
        leaves.push_back({field_path, sb.GetString()});
    } else {
        // Object body → walk the tree; field_path is the subtree root
        collectLeaves(doc, field_path, leaves);
    }

    if (leaves.empty()) {
        t_res.status = 400;
        t_res.set_content(R"({"error":"JSON body produced no writable leaves"})", "application/json");
        return;
    }

    for (const auto& leaf : leaves) {
        if (!isAuthorizedField(t_req, db_num, leaf.first, true)) {
            t_res.status = 403;
            t_res.set_content(fmt::format(R"({{"error":"Forbidden: Not authorised to write path '{}'"}})", leaf.first), "application/json");
            return;
        }
    }

    // ── 5. Atomic-batch write ─────────────────────────────────────────────
    // All leaves share the same timestamp so processDirty() handles them as
    // one coherent batch — effectively atomic from the arena's perspective.
    const uint64_t ts = static_cast<uint64_t>(sgrn::utils::time::nowMilliseconds());

    int written = 0;
    for (const auto& [leaf_path, leaf_value] : leaves) {
        auto r = t_memory.updateFieldWithTimestamp(db_num, leaf_path, leaf_value, ts);
        if (!r.hasError())
            ++written;
    }

    if (written == 0) {
        t_res.status = 422;
        t_res.set_content(R"({"error":"No fields were written — verify the paths and value types"})", "application/json");
        return;
    }

    // ── 6. Read back the written value and include it in the response ──────
    t_memory.processor()->processCommands();
    std::string current_value;
    if (field_path.empty()) {
        current_value = t_memory.getDbJsonString(db_num);
    } else {
        auto r = t_memory.getFieldValue(db_num, field_path);
        current_value = r.hasError() ? "null" : r.value();
    }

    // Build response: splice the raw JSON value directly (no double-encoding)
    t_res.status = 200;
    t_res.set_content(fmt::format(R"({{"db":{},"path":"{}","fields_written":{},"value":{}}})", db_num, url_path, written, current_value),
        "application/json");
}

} // namespace sgrn::gateway::adapters
