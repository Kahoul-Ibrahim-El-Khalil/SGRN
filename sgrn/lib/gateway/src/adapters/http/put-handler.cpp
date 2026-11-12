#include <fmt/core.h>
#include <sgrn/gateway/adapters/http.hpp>
#include <sgrn/gateway/adapters/http/errors.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/json-dom.hpp>
#include <sgrn/utils/json.hpp>
#include <stdexcept>
#include <string>

namespace sgrn::gateway::adapters
{

namespace
{
std::string buildRawWriteResponse(uint16_t t_db, size_t t_offset, size_t t_size, const uint8_t* tp_data) {
    return fmt::format(
        R"({{"t_db":{},"t_offset":{},"t_size":{},"written":"{}"}})", t_db, t_offset, t_size, sgrn::utils::encoding::toHex(tp_data, t_size));
}
} // namespace

void HttpAdapter::handlePut(const http::HttpRequest& t_req, http::HttpResponse& t_res) {
    PlcMemory& t_memory = *refs_.memory;
    // ── 1. Check if it's a multi-write request (JSON array) ──────────────
    if (!t_req.has_param("db") && !t_req.has_param("offset") && !t_req.has_param("size")) {
        const std::string& ct = t_req.get_header_value("Content-Type");
        if (ct.find("application/json") == std::string::npos) {
            t_res.status = 400;
            t_res.set_content(
                R"json({"error":"PUT requires either ?t_db=<n>&t_offset=<o>&t_size=<s> or a JSON array payload (Content-Type: application/json)"})json",
                "application/json");
            return;
        }

        sgrn::json::dom::Document doc;
        doc.parse(t_req.body.c_str());
        if (doc.hasParseError() || !doc.is_array()) {
            t_res.status = 400;
            t_res.set_content(R"json({"error":"PUT multi-write requires a JSON array"})json", "application/json");
            return;
        }

        // ── Multi-write: collect all items first, then batch write ────────────
        std::vector<twin::DbMemorySpan> write_spans;
        std::vector<std::vector<uint8_t>> payloads;
        std::vector<std::tuple<uint16_t, size_t, size_t>> item_metadata; // db, offset, size for response

        // First pass: validate and collect all items
        for (sgrn::json::dom::SizeType i = 0; i < doc.size(); i++) {
            const auto& item = doc[i];
            if (!item.is_object() || !item.hasMember("db") || !item.hasMember("offset") || !item.hasMember("size") ||
                !item.hasMember("data")) {
                t_res.status = 400;
                t_res.set_content(
                    R"json({"error":"Each item must have t_db, t_offset, t_size, and tp_data (base64)"})json", "application/json");
                return;
            }

            uint16_t db_num = static_cast<uint16_t>(item["db"].getUint());
            size_t item_offset = item["offset"].getUint64();
            size_t item_size = item["size"].getUint64();
            std::string data_b64 = item["data"].getString();

            const std::string& client_ip = t_req.remote_ip;
            if (!isAuthorized(t_req, db_num)) {
                t_res.status = 403;
                t_res.set_content(
                    fmt::format(R"({{"error":"IP {} is not authorised to write DB{}"}})", client_ip, db_num), "application/json");
                return;
            }

            std::vector<uint8_t> payload = ::sgrn::utils::encoding::fromBase64(data_b64);
            if (payload.size() < item_size) {
                t_res.status = 400;
                t_res.set_content(
                    fmt::format(R"({{"error":"Base64 decodes to {} bytes but t_size={} was requested"}})", payload.size(), item_size),
                    "application/json");
                return;
            }
            payload.resize(item_size);

            // Collect for batch write
            payloads.push_back(std::move(payload));
            item_metadata.push_back(std::make_tuple(db_num, item_offset, item_size));
            write_spans.push_back({.db = db_num, .offset = item_offset, .size = item_size, .p_buffer = payloads.back().data()});
        }

        // Second pass: atomic batch write (all or nothing)
        if (auto r = t_memory.writeDbMemory(std::span(write_spans)); !r) {
            t_res.status = 413;
            t_res.set_content(
                fmt::format(R"json({{"error":"Batch write failed: {}","items":{},"reason":"atomic write failed, no items written"}})json",
                    r.error(), doc.size()),
                "application/json");
            return;
        }

        // Third pass: build success response
        int written_count = 0;
        sgrn::json::dom::Document response_array;
        auto& response_alloc = response_array.allocator();
        response_array.setArray();

        for (size_t i = 0; i < payloads.size(); i++) {
            const auto& [db_num, item_offset, item_size] = item_metadata[i];
            const std::string written_hex = sgrn::utils::encoding::toHex(payloads[i].data(), item_size);
            sgrn::json::dom::Value res_item(sgrn::json::dom::Type::Object);
            res_item.addMember("db", db_num, response_alloc);
            res_item.addMember("offset", item_offset, response_alloc);
            res_item.addMember("size", item_size, response_alloc);
            res_item.addMember("written", sgrn::json::dom::Value(written_hex.c_str(), response_alloc), response_alloc);
            response_array.pushBack(res_item, response_alloc);
            written_count++;
        }

        sgrn::json::dom::Document root;
        auto& root_alloc = root.allocator();
        root.setObject();
        root.addMember("items_written", written_count, root_alloc);
        sgrn::json::dom::Value results_copy;
        results_copy.copyFrom(response_array, root_alloc);
        root.addMember("results", results_copy, root_alloc);

        t_res.status = 200;
        t_res.set_content(sgrn::utils::json::serializeCompact(root), "application/json");
        return;
    }

    // ── 2. Single item write (Mandatory query parameters) ───────────────────
    if (!t_req.has_param("db") || !t_req.has_param("offset") || !t_req.has_param("size")) {
        t_res.status = 400;
        t_res.set_content(R"({"error":"PUT requires query params: ?t_db=<n>&t_offset=<o>&t_size=<s>"})", "application/json");
        return;
    }

    uint16_t db_num = 0;
    size_t t_offset = 0;
    size_t t_size = 0;
    try {
        db_num = static_cast<uint16_t>(std::stoul(t_req.get_param_value("db")));
        t_offset = std::stoull(t_req.get_param_value("offset"));
        t_size = std::stoull(t_req.get_param_value("size"));
    } catch (const std::invalid_argument& e) {
        t_res.status = 400;
        t_res.set_content(R"({"error":"t_db, t_offset and t_size must be valid numbers"})", "application/json");
        return;
    } catch (const std::out_of_range& e) {
        t_res.status = 400;
        t_res.set_content(R"({"error":"t_db, t_offset and t_size must be within range"})", "application/json");
        return;
    }

    if (t_size == 0) {
        t_res.status = 400;
        t_res.set_content(R"({"error":"t_size must be > 0"})", "application/json");
        return;
    }

    // ── 2. IP / DB ACL ────────────────────────────────────────────────────
    const std::string& client_ip = t_req.remote_ip;
    if (!isAuthorized(t_req, db_num)) {
        t_res.status = 403;
        t_res.set_content(fmt::format(R"({{"error":"IP {} is not authorised to write DB{}"}})", client_ip, db_num), "application/json");
        return;
    }

    // ── 3. Decode body ────────────────────────────────────────────────────
    std::vector<uint8_t> payload;
    const std::string& ct = t_req.get_header_value("Content-Type");

    if (ct.find("text/plain") != std::string::npos) {
        // Body is a base64 string.  Decode first, then honour ?size= as the
        // expected decoded byte count (allows caller to strip padding artefacts).
        payload = sgrn::utils::encoding::fromBase64(t_req.body);

        if (payload.size() < t_size) {
            t_res.status = 400;
            t_res.set_content(
                fmt::format(R"({{"error":"Base64 decodes to {} bytes but t_size={} was requested"}})", payload.size(), t_size),
                "application/json");
            return;
        }
        // Trim to the exact declared size (decoded padding bytes are discarded)
        payload.resize(t_size);

    } else {
        // application/octet-stream (or unspecified) → treat body as raw bytes
        payload.assign(
            reinterpret_cast<const uint8_t*>(t_req.body.data()), reinterpret_cast<const uint8_t*>(t_req.body.data()) + t_req.body.size());

        if (payload.size() < t_size) {
            t_res.status = 400;
            t_res.set_content(
                fmt::format(R"({{"error":"Body is {} bytes but t_size={} was requested"}})", payload.size(), t_size), "application/json");
            return;
        }
        payload.resize(t_size); // honour the declared window
    }

    // ── 4. Write to PLC arena ─────────────────────────────────────────────
    // writeDbMemory() enforces: offset + size <= DB.size_bytes
    if (auto r = t_memory.writeDbMemory(db_num, t_offset, t_size, payload.data()); !r) {
        // Unified mapping: PlcMemoryError -> HTTP status.
        t_res.status = http::toHttpStatus(static_cast<twin::PlcMemoryError>(r.error()));
        t_res.set_content(fmt::format(R"json({{"error":"Write to DB{} t_offset={} t_size={} failed: {}"}})json", db_num, t_offset, t_size,
                              toString(r.error())),
            "application/json");
        return;
    }

    // ── 5. Respond with written bytes summary ─────────────────────────────
    t_res.status = 200;
    t_res.set_content(buildRawWriteResponse(db_num, t_offset, t_size, payload.data()), "application/json");
}

} // namespace sgrn::gateway::adapters
