#include <fmt/core.h>

#include <sgrn/gateway/adapters/http.hpp>
#include <sgrn/gateway/adapters/http/errors.hpp>
#include <sgrn/gateway/adapters/http/path.hpp>
#include <sgrn/gateway/twin/PlcCommandProcessor.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/utils/time.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sgrn::gateway::adapters
{

using sgrn::utils::time::nowMilliseconds;

namespace
{

// -----------------------------------------------------------------------------
// Path utilities
// -----------------------------------------------------------------------------

// Split a slash-separated path without allocating each individual segment.
//
// Example:
//   "towers/1/moisture_loading"
//      -> {"towers", "1", "moisture_loading"}
//
// The returned string_views refer to the original string, so the caller must
// keep it alive for as long as the returned vector is used.
std::vector<std::string_view> splitSlashes(std::string_view path) {
    std::vector<std::string_view> segments;

    if (path.empty()) {
        segments.emplace_back();
        return segments;
    }

    size_t begin = 0;

    while (begin <= path.size()) {
        const size_t end = path.find('/', begin);

        if (end == std::string_view::npos) {
            segments.emplace_back(path.substr(begin));
            break;
        }

        segments.emplace_back(path.substr(begin, end - begin));
        begin = end + 1;

        // Preserve a trailing empty segment, matching the old implementation.
        if (begin == path.size()) {
            segments.emplace_back();
            break;
        }
    }

    return segments;
}

std::string joinSlashes(std::span<const std::string_view> segments, size_t begin, size_t end) {
    end = std::min(end, segments.size());

    if (begin >= end)
        return {};

    size_t size = 0;

    for (size_t i = begin; i < end; ++i) {
        size += segments[i].size();

        if (i + 1 < end)
            ++size;
    }

    std::string result;
    result.reserve(size);

    for (size_t i = begin; i < end; ++i) {
        if (i != begin)
            result.push_back('/');

        result.append(segments[i]);
    }

    return result;
}

// Parse a path segment as an array index.
//
// Using from_chars avoids exceptions and does not require constructing a
// temporary std::string.
std::optional<size_t> parseIndex(std::string_view segment) {
    if (segment.empty())
        return std::nullopt;

    size_t value = 0;

    const auto [ptr, ec] = std::from_chars(segment.data(), segment.data() + segment.size(), value);

    if (ec != std::errc{} || ptr != segment.data() + segment.size())
        return std::nullopt;

    return value;
}

// -----------------------------------------------------------------------------
// Array element paths
// -----------------------------------------------------------------------------

// Split
//
//     towers/1/moisture_loading
//
// into
//
//     array_path = "towers"
//     index      = 1
//     rest       = "moisture_loading"
//
// Returns false when there is no numeric segment addressing an array element.
//
// Deeper nesting such as:
//
//     matrix/1/2/value
//
// is resolved one level at a time.
struct ArrayElementRef {
    std::string array_path;
    size_t index = 0;
    std::string rest;
};

bool splitArrayElement(std::string_view leaf_path, ArrayElementRef& result) {
    const auto segments = splitSlashes(leaf_path);

    for (size_t i = 1; i < segments.size(); ++i) {
        const auto index = parseIndex(segments[i]);

        if (!index)
            continue;

        const std::string rest = joinSlashes(segments, i + 1, segments.size());

        // A numeric segment without anything following it is not considered
        // a nested leaf path here.
        if (rest.empty())
            return false;

        result.array_path = joinSlashes(segments, 0, i);

        result.index = *index;
        result.rest = rest;

        return true;
    }

    return false;
}

// -----------------------------------------------------------------------------
// RapidJSON helpers
// -----------------------------------------------------------------------------

std::string serializeJson(const rapidjson::Value& value) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);

    value.Accept(writer);

    return buffer.GetString();
}

// Navigate `root` (which must be an array) to `index`, then descend `rest`.
//
// Numeric path segments address array elements.
// Non-numeric path segments address object members.
//
// Every segment must already exist.
bool setNestedLeaf(rapidjson::Value& root, size_t index, std::string_view rest, const rapidjson::Value& value,
    rapidjson::Document::AllocatorType& allocator, std::string& error) {
    if (!root.IsArray()) {
        error = "target is not a JSON array";
        return false;
    }

    if (index >= root.Size()) {
        error = "array index out of range";
        return false;
    }

    rapidjson::Value* current = &root[static_cast<rapidjson::SizeType>(index)];

    const auto segments = splitSlashes(rest);

    for (size_t i = 0; i < segments.size(); ++i) {
        const bool last = i + 1 == segments.size();
        const std::string_view segment = segments[i];

        if (const auto nested_index = parseIndex(segment)) {
            if (!current->IsArray()) {
                error = "segment '" + std::string(segment) + "' addresses a non-array";

                return false;
            }

            if (*nested_index >= current->Size()) {
                error = "nested array index out of range";
                return false;
            }

            auto& child = (*current)[static_cast<rapidjson::SizeType>(*nested_index)];

            if (last) {
                child.CopyFrom(value, allocator);
                return true;
            }

            current = &child;
            continue;
        }

        const std::string key(segment);

        if (!current->IsObject() || !current->HasMember(key.c_str())) {
            error = "unknown field '" + std::string(segment) + "'";

            return false;
        }

        auto& child = (*current)[key.c_str()];

        if (last) {
            child.CopyFrom(value, allocator);
            return true;
        }

        current = &child;
    }

    error = "empty sub-path";
    return false;
}

// -----------------------------------------------------------------------------
// Leaf writing
// -----------------------------------------------------------------------------

struct LeafWriteOutcome {
    int written = 0;

    std::string element_readback;
    bool has_element_readback = false;
};

struct ArrayWriteGroup {
    struct Operation {
        size_t index;
        std::string rest;
        std::string value;
    };

    std::vector<Operation> operations;
};

struct DirectLeaf {
    std::string path;
    std::string value;
};

// Write a batch of (leaf_path, json_value) leaves.
//
// Directly addressable fields are written individually.
//
// Struct-array elements such as:
//
//     towers/1/moisture_loading
//
// are grouped by array and applied using one read-modify-write operation per
// array.
LeafWriteOutcome applyLeafWrites(
    twin::PlcMemory& memory, uint16_t db, const std::vector<std::pair<std::string, std::string>>& leaves, uint64_t timestamp) {
    LeafWriteOutcome outcome;

    std::map<std::string, ArrayWriteGroup> array_groups;
    std::vector<DirectLeaf> direct_leaves;

    // -------------------------------------------------------------------------
    // Classify writes
    // -------------------------------------------------------------------------

    for (const auto& [leaf_path, leaf_value] : leaves) {
        // Fast path: directly addressable node.
        if (memory.findSymbol(db, leaf_path) != nullptr) {
            direct_leaves.push_back({leaf_path, leaf_value});

            continue;
        }

        ArrayElementRef ref;

        if (!splitArrayElement(leaf_path, ref)) {
            // Unknown path.
            continue;
        }

        // Validate the array against the live node tree.
        const twin::PlcNode* array_node = memory.findSymbol(db, ref.array_path);

        if (array_node == nullptr || array_node->count_ <= 1 || ref.index >= static_cast<size_t>(array_node->count_)) {
            continue;
        }

        array_groups[ref.array_path].operations.push_back({ref.index, std::move(ref.rest), leaf_value});
    }

    // -------------------------------------------------------------------------
    // Direct writes
    // -------------------------------------------------------------------------

    for (const auto& leaf : direct_leaves) {
        auto result = memory.updateFieldWithTimestamp(db, leaf.path, leaf.value, timestamp);

        if (!result.hasError())
            ++outcome.written;
    }

    // -------------------------------------------------------------------------
    // Struct-array writes
    // -------------------------------------------------------------------------

    for (auto& [array_path, group] : array_groups) {
        auto array_result = memory.getFieldValue(db, array_path);

        if (array_result.hasError())
            continue;

        rapidjson::Document array_document;

        if (array_document.Parse(array_result.value().c_str()).HasParseError() || !array_document.IsArray()) {
            continue;
        }

        int group_applied = 0;

        for (const auto& operation : group.operations) {
            if (operation.index >= array_document.Size()) {
                // The live array may have been resized since validation.
                continue;
            }

            rapidjson::Document value_document;

            if (value_document.Parse(operation.value.c_str()).HasParseError())
                continue;

            std::string error;

            if (!setNestedLeaf(array_document, operation.index, operation.rest, value_document, array_document.GetAllocator(), error)) {
                continue;
            }

            ++outcome.written;
            ++group_applied;
        }

        if (group_applied == 0)
            continue;

        const std::string new_array_json = serializeJson(array_document);

        auto write_result = memory.updateFieldWithTimestamp(db, array_path, new_array_json, timestamp);

        if (write_result.hasError()) {
            // The whole-array write failed to queue.
            outcome.written -= group_applied;
            outcome.written = std::max(outcome.written, 0);

            continue;
        }

        // Read back the last touched element.
        const auto& last = group.operations.back();

        if (last.index < array_document.Size()) {
            outcome.element_readback = serializeJson(array_document[static_cast<rapidjson::SizeType>(last.index)]);

            outcome.has_element_readback = true;
        }
    }

    return outcome;
}

// -----------------------------------------------------------------------------
// Indexed array read-modify-write
// -----------------------------------------------------------------------------

struct ArrayUpdateResult {
    std::string serialized_array;
    std::string serialized_element;
};

std::optional<ArrayUpdateResult> replaceArrayElement(std::string_view array_json, size_t index, const rapidjson::Value& replacement) {
    rapidjson::Document array_document;

    if (array_document.Parse(array_json.data(), array_json.size()).HasParseError() || !array_document.IsArray() ||
        index >= array_document.Size()) {
        return std::nullopt;
    }

    auto& element = array_document[static_cast<rapidjson::SizeType>(index)];

    element.CopyFrom(replacement, array_document.GetAllocator());

    return ArrayUpdateResult{.serialized_array = serializeJson(array_document), .serialized_element = serializeJson(element)};
}

} // namespace

void HttpAdapter::handlePost(const http::HttpRequest& request, http::HttpResponse& response) {
    const PlcSchemaStore& registry = *refs_.registry;
    PlcMemory& memory = *refs_.memory;

    // -------------------------------------------------------------------------
    // 1. Resolve URL path -> DB schema + relative field path
    // -------------------------------------------------------------------------

    std::string url_path = request.path;

    if (!url_path.empty() && url_path.back() == '/')
        url_path.pop_back();

    // -------------------------------------------------------------------------
    // Multi-DB POST
    // -------------------------------------------------------------------------

    if (url_path.empty()) {
        // The body must be a JSON object where keys are DB names.
        rapidjson::Document document;
        document.Parse(request.body.c_str());

        if (document.HasParseError() || !document.IsObject()) {
            response.status = 400;
            response.set_content(R"({"error":"POST /data/ requires a JSON object with DB names as keys"})", "application/json");

            return;
        }

        struct PendingLeaf {
            uint16_t db;
            std::string path;
            std::string value;
        };

        std::vector<PendingLeaf> all_leaves;

        const std::string& client_ip = request.remote_ip;

        for (auto it = document.MemberBegin(); it != document.MemberEnd(); ++it) {
            const std::string db_name = it->name.GetString();

            auto result = registry.getDbByName(db_name);

            if (result.hasError() || !result.value()) {
                response.status = 404;

                response.set_content(fmt::format(R"({{"error":"Unknown DB '{}'"}})", db_name), "application/json");

                return;
            }

            const uint16_t db_num = result.value()->db_number;

            std::vector<std::pair<std::string, std::string>> db_leaves;

            collectLeaves(it->value, "", db_leaves);

            for (const auto& leaf : db_leaves) {
                if (!isAuthorizedField(request, db_num, leaf.first, true)) {
                    response.status = 403;

                    response.set_content(fmt::format(R"({{"error":"Forbidden: IP {} is not authorised to write path '{}' in DB{}"}})",
                                             client_ip, leaf.first, db_num),
                        "application/json");

                    return;
                }

                all_leaves.push_back({db_num, leaf.first, leaf.second});
            }
        }

        if (all_leaves.empty()) {
            response.status = 400;

            response.set_content(R"({"error":"JSON body produced no writable leaves"})", "application/json");

            return;
        }

        const uint64_t timestamp = static_cast<uint64_t>(nowMilliseconds());

        // Group leaves by DB.
        std::map<uint16_t, std::vector<std::pair<std::string, std::string>>> leaves_by_db;

        for (const auto& leaf : all_leaves) {
            leaves_by_db[leaf.db].push_back({leaf.path, leaf.value});
        }

        int written = 0;

        for (auto& [db_num, db_leaves] : leaves_by_db) {
            written += applyLeafWrites(memory, db_num, db_leaves, timestamp).written;
        }

        if (written == 0) {
            response.status = 422;

            response.set_content(R"({"error":"No fields were written — verify the paths and value types"})", "application/json");

            return;
        }

        memory.processor()->processCommands();

        response.status = 200;

        response.set_content(fmt::format(R"({{"fields_written":{}}})", written), "application/json");

        return;
    }

    auto [schema, field_path, array_index] = resolveSemanticPath(sgrn::utils::strings::tokenize(url_path, '/'), registry);

    if (!schema) {
        response.status = 404;

        response.set_content(fmt::format(R"({{"error":"Path '{}' does not resolve to a known DB"}})", url_path), "application/json");

        return;
    }

    const uint16_t db_num = schema->db_number;

    // -------------------------------------------------------------------------
    // 2. Parse JSON body
    // -------------------------------------------------------------------------

    rapidjson::Document document;
    document.Parse(request.body.c_str());

    if (document.HasParseError()) {
        response.status = 400;

        response.set_content(R"({"error":"Body is not valid JSON"})", "application/json");

        return;
    }

    // -------------------------------------------------------------------------
    // 3. Indexed array element write
    //
    // POST /data/<DB>/<field>/<N>
    //
    // Scalar body:
    //     replace element N
    //
    // Object body:
    //     replace an element of an array-of-struct
    // -------------------------------------------------------------------------

    if (array_index.has_value()) {
        const size_t index = *array_index;

        if (document.IsObject()) {
            // -------------------------------------------------------------
            // Array-of-struct element replacement
            // -------------------------------------------------------------

            const twin::PlcNode* array_node = memory.findSymbol(db_num, field_path);

            const bool struct_array = array_node != nullptr && array_node->count_ > 1 && !array_node->children_.empty();

            if (!struct_array) {
                response.status = 400;

                response.set_content(
                    R"({"error":"Indexed array write expects a scalar or non-object value, not a JSON object"})", "application/json");

                return;
            }

            if (index >= static_cast<size_t>(array_node->count_)) {
                response.status = 416;

                response.set_content(
                    fmt::format(R"X({{"error":"Array index {} out of range (size={})"}})X", index, array_node->count_), "application/json");

                return;
            }

            if (!isAuthorizedField(request, db_num, field_path, true)) {
                response.status = 403;

                response.set_content(
                    fmt::format(R"({{"error":"Forbidden: Not authorised to write path '{}'"}})", field_path), "application/json");

                return;
            }

            auto array_result = memory.getFieldValue(db_num, field_path);

            if (array_result.hasError()) {
                response.status = http::toHttpStatus(array_result.error());

                response.set_content(fmt::format(R"({{"error":"Array field '{}' not found"}})", field_path), "application/json");

                return;
            }

            const auto update = replaceArrayElement(array_result.value(), index, document);

            if (!update) {
                response.status = 422;

                response.set_content(
                    fmt::format(R"({{"error":"Field '{}' is not a JSON array or index out of range"}})", field_path), "application/json");

                return;
            }

            const uint64_t timestamp = static_cast<uint64_t>(nowMilliseconds());

            auto write_result = memory.updateFieldWithTimestamp(db_num, field_path, update->serialized_array, timestamp);

            if (write_result.hasError()) {
                response.status = http::toHttpStatus(write_result.error());

                response.set_content(
                    fmt::format(R"({{"error":"Failed to write array field '{}': {}"}})", field_path, toString(write_result.error())),
                    "application/json");

                return;
            }

            memory.processor()->processCommands();

            response.status = 200;

            response.set_content(
                fmt::format(R"({{"db":{},"path":"{}","index":{},"value":{}}})", db_num, url_path, index, update->serialized_element),
                "application/json");

            return;
        }

        // ---------------------------------------------------------------------
        // Scalar / primitive array element replacement
        // ---------------------------------------------------------------------

        if (field_path.empty()) {
            response.status = 400;

            response.set_content(R"({"error":"Indexed array write requires a non-empty field path"})", "application/json");

            return;
        }

        if (!isAuthorizedField(request, db_num, field_path, true)) {
            response.status = 403;

            response.set_content(
                fmt::format(R"({{"error":"Forbidden: Not authorised to write path '{}'"}})", field_path), "application/json");

            return;
        }

        auto array_result = memory.getFieldValue(db_num, field_path);

        if (array_result.hasError()) {
            response.status = http::toHttpStatus(array_result.error());

            response.set_content(fmt::format(R"({{"error":"Array field '{}' not found"}})", field_path), "application/json");

            return;
        }

        const auto update = replaceArrayElement(array_result.value(), index, document);

        if (!update) {
            // Preserve the original distinction between an invalid array and
            // an out-of-range index where possible.
            rapidjson::Document array_document;

            if (array_document.Parse(array_result.value().c_str()).HasParseError() || !array_document.IsArray()) {
                response.status = 422;

                response.set_content(fmt::format(R"({{"error":"Field '{}' is not a JSON array"}})", field_path), "application/json");

                return;
            }

            response.status = 416;

            response.set_content(
                fmt::format(R"X({{"error":"Array index {} out of range (size={})"}})X", index, array_document.Size()), "application/json");

            return;
        }

        const uint64_t timestamp = static_cast<uint64_t>(nowMilliseconds());

        auto write_result = memory.updateFieldWithTimestamp(db_num, field_path, update->serialized_array, timestamp);

        if (write_result.hasError()) {
            response.status = http::toHttpStatus(write_result.error());

            response.set_content(
                fmt::format(R"({{"error":"Failed to write array field '{}': {}"}})", field_path, toString(write_result.error())),
                "application/json");

            return;
        }

        memory.processor()->processCommands();

        response.status = 200;

        response.set_content(
            fmt::format(R"({{"db":{},"path":"{}","index":{},"value":{}}})", db_num, url_path, index, update->serialized_element),
            "application/json");

        return;
    }

    // -------------------------------------------------------------------------
    // 4. Collect leaf writes
    // -------------------------------------------------------------------------

    std::vector<std::pair<std::string, std::string>> leaves;

    if (!document.IsObject()) {
        // Scalar or array body -> write directly to the resolved field path.

        if (field_path.empty()) {
            response.status = 400;

            response.set_content(R"({"error":"Scalar body requires a path pointing to a leaf field"})", "application/json");

            return;
        }

        leaves.emplace_back(field_path, serializeJson(document));
    } else {
        // Object body -> walk the tree.
        collectLeaves(document, field_path, leaves);
    }

    if (leaves.empty()) {
        response.status = 400;

        response.set_content(R"({"error":"JSON body produced no writable leaves"})", "application/json");

        return;
    }

    // -------------------------------------------------------------------------
    // 5. Authorization
    // -------------------------------------------------------------------------

    for (const auto& [path, value] : leaves) {
        if (!isAuthorizedField(request, db_num, path, true)) {
            response.status = 403;

            response.set_content(fmt::format(R"({{"error":"Forbidden: Not authorised to write path '{}'"}})", path), "application/json");

            return;
        }
    }

    // -------------------------------------------------------------------------
    // 6. Atomic batch write
    //
    // All leaves share the same timestamp so processDirty() handles them as
    // one coherent batch.
    //
    // Struct-array elements such as:
    //
    //     towers/1/moisture_loading
    //
    // are applied using one read-modify-write per array.
    // -------------------------------------------------------------------------

    const uint64_t timestamp = static_cast<uint64_t>(nowMilliseconds());

    const LeafWriteOutcome outcome = applyLeafWrites(memory, db_num, leaves, timestamp);

    const int written = outcome.written;

    if (written == 0) {
        response.status = 422;

        response.set_content(R"({"error":"No fields were written — verify the paths and value types"})", "application/json");

        return;
    }

    // -------------------------------------------------------------------------
    // 7. Read back the written value
    // -------------------------------------------------------------------------

    memory.processor()->processCommands();

    std::string current_value;

    if (outcome.has_element_readback) {
        current_value = outcome.element_readback;
    } else if (field_path.empty()) {
        current_value = memory.getDbJsonString(db_num);
    } else {
        auto result = memory.getFieldValue(db_num, field_path);

        current_value = result.hasError() ? "null" : result.value();
    }

    // The value is already serialized JSON, so insert it directly rather than
    // JSON-encoding it as a string.
    response.status = 200;

    response.set_content(fmt::format(R"({{"db":{},"path":"{}","fields_written":{},"value":{}}})", db_num, url_path, written, current_value),
        "application/json");
}

} // namespace sgrn::gateway::adapters
