/* sgrn/gateway/src/adapters/http/memory_handlers.cpp */
/**
 * @file memory_handlers.cpp
 * @brief REST API handlers for raw binary memory access
 *
 * This module implements the `/memory/\*` endpoints, which provide low-level,
 * byte-addressable access to PLC memory organized by database (DB) number,
 * byte offset, and size.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * DESIGN: Binary vs. Base64URL Encoding
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * Two encoding modes are supported:
 *
 *   1. RAW BINARY (Single DB only)
 *      └─ GET/PUT /memory/db/<db>/offset/<off>/size/<sz>
 *      └─ Request/response body is raw bytes (application/octet-stream)
 *      └─ Constraint: Single DB per request ensures correct C++ struct casting
 *      └─ S7 semantics: direct byte-for-byte memory copy
 *
 *   2. BASE64URL (Multiple DBs via batch)
 *      └─ GET/PUT /memory/batch
 *      └─ Request/response body is JSON array (Content-Type: application/json)
 *      └─ Each item: {db, offset, size, data: "base64url"}
 *      └─ Atomicity: All spans or nothing (uses PlcMemory batch span API)
 *      └─ One lock per unique DB (optimized for multi-DB transactions)
 *
 * Web clients wishing to support S7 semantics should:
 *   - Use binary mode (GET /memory/db/) for single-DB reads/writes
 *   - Use batch mode (PUT /memory/batch) for multi-DB atomic writes
 *   - Encode/decode base64url for JSON mode, raw bytes for binary mode
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * REST API Reference
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * [GET] /memory/db/<db>/offset/<off>/size/<sz>
 *   Description: Read raw bytes from a single database.
 *   Path params:
 *     db    - Database number (uint16_t)
 *     off   - Byte offset within the DB (size_t)
 *     sz    - Number of bytes to read (size_t, must be > 0)
 *
 *   Success response (200):
 *     Content-Type: application/octet-stream
 *     Body: raw bytes (exactly <sz> bytes)
 *
 *   SchemaError responses:
 *     400 - Missing or invalid path parameters
 *     403 - Client IP not authorized for this DB
 *     404 - DB not found or offset+size exceeds DB bounds
 *     413 - Read failed (e.g., PLC communication error)
 *
 *   Example:
 *     GET /memory/db/1/offset/0/size/4
 *     → (200) [raw 4 bytes: 0x01 0x02 0x03 0x04]
 *
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * [PUT] /memory/db/<db>/offset/<off>/size/<sz>
 *   Description: Write raw bytes to a single database (atomic).
 *   Path params: (same as GET)
 *
 *   Request:
 *     Content-Type: application/octet-stream
 *     Body: raw bytes (must be exactly <sz> bytes; padding/truncation rejected)
 *
 *   Success response (200):
 *     Content-Type: application/octet-stream
 *     Body: echoed raw bytes (confirms write; S7 semantics)
 *
 *   SchemaError responses: (same as GET)
 *
 *   Example:
 *     PUT /memory/db/1/offset/0/size/4
 *     Content-Type: application/octet-stream
 *     [raw 4 bytes: 0x01 0x02 0x03 0x04]
 *     → (200) [echoed 4 bytes]
 *
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * [GET] /memory/batch?db=<n1>&offset=<o1>&size=<s1>&db=<n2>&offset=<o2>&size=<s2>&...
 *   Description: Read from multiple databases in a single atomic batch.
 *   Query params (repeatable):
 *     db     - Database number (uint16_t, repeatable)
 *     offset - Byte offset for corresponding DB (size_t, repeatable)
 *     size   - Byte count for corresponding DB (size_t, repeatable)
 *     Triplets are matched by repetition order: 1st db uses 1st offset/size, etc.
 *
 *   Success response (200):
 *     Content-Type: application/json
 *     Body: JSON array of read items, base64url-encoded
 *     [
 *       {"db": 1, "offset": 0, "size": 4, "data": "AQIDBA=="},
 *       {"db": 2, "offset": 8, "size": 2, "data": "AQ=="}
 *     ]
 *
 *   SchemaError responses:
 *     400 - Missing, invalid, or mismatched triplets
 *     403 - Client IP not authorized for any DB
 *     404 - Any DB not found or range exceeds bounds
 *     413 - Batch read failed (entire batch is atomic)
 *
 *   Example:
 *     GET /memory/batch?db=1&offset=0&size=4&db=2&offset=8&size=2
 *     → (200) [JSON array with base64url data]
 *
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * [PUT] /memory/batch
 *   Description: Write to multiple databases in a single atomic batch.
 *   Semantics: All-or-nothing — if any span fails, nothing is written.
 *   Uses PlcMemory batch span API for optimized locking (one lock per DB).
 *
 *   Request:
 *     Content-Type: application/json
 *     Body: JSON array
 *     [
 *       {"db": 1, "offset": 0, "size": 4, "data": "AQIDBA=="},
 *       {"db": 2, "offset": 8, "size": 2, "data": "AQ=="}
 *     ]
 *
 *   Success response (200):
 *     Content-Type: application/json
 *     Body: Same structure, confirms written (data → written)
 *     [
 *       {"db": 1, "offset": 0, "size": 4, "written": "AQIDBA=="},
 *       {"db": 2, "offset": 8, "size": 2, "written": "AQ=="}
 *     ]
 *
 *   SchemaError responses:
 *     400 - Invalid JSON or missing required fields
 *     403 - Client IP not authorized for any DB
 *     404 - Any DB not found or range exceeds bounds
 *     413 - Batch write failed (ATOMIC: no items were written)
 *           Response includes error reason and item count
 *           {"error": "<PlcMemoryError>", "items": 2, "reason": "atomic write failed, no items written"}
 *
 *   Example:
 *     PUT /memory/batch
 *     Content-Type: application/json
 *     [
 *       {"db": 1, "offset": 0, "size": 4, "data": "AQIDBA=="},
 *       {"db": 2, "offset": 8, "size": 2, "data": "AQ=="}
 *     ]
 *     → (200) [JSON array with "written" fields]
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Implementation Notes
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * - All offsets/sizes are validated to ensure offset + size <= DB.size_bytes
 * - Base64URL encoding: alphabet [A-Z a-z 0-9 - _] (not standard base64)
 * - Batch operations use PlcMemory::readDbMemory(std::span<DbMemorySpan>)
 *   and PlcMemory::writeDbMemory(std::span<DbMemorySpan>) for atomicity
 * - Client IP ACL checked against same table as semantic API
 * - All errors include PlcMemoryError details for client debugging
 */

#include <fmt/core.h>
#include <sgrn/debug.hpp>
#include <sgrn/gateway/adapters/http.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/json.hpp>
#include <sgrn/utils/strings.hpp>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <vector>

namespace sgrn::gateway::adapters
{

/**
 * @brief Parses path parameters: /memory/db/<db>/offset/<off>/size/<sz>
 * @returns Tuple of (db, offset, size) or error tuple (-1, 0, 0) on failure
 */
static std::tuple<int, size_t, size_t> parseMemoryPath(const std::string& t_path) {
    std::vector<std::string> parts = sgrn::utils::strings::tokenize(t_path, '/');
    // Accept both:
    //   "1/offset/0/size/72"   (the current httplib capture from /memory/db/(.*))
    //   "/db/1/offset/0/size/72" (legacy/internal callers)
    const bool has_db_prefix = parts.size() == 7 && parts[1] == "db" && parts[3] == "offset" && parts[5] == "size";
    const bool bare_path = parts.size() == 5 && parts[1] == "offset" && parts[3] == "size";
    if (!has_db_prefix && !bare_path) {
        return std::make_tuple(-1, 0, 0);
    }

    try {
        const size_t db_idx = has_db_prefix ? 2 : 0;
        const size_t off_idx = has_db_prefix ? 4 : 2;
        const size_t size_idx = has_db_prefix ? 6 : 4;

        unsigned long db_raw = std::stoul(parts[db_idx]);
        if (db_raw > 65535U)
            return std::make_tuple(-1, 0, 0);
        int db = static_cast<int>(db_raw);
        size_t offset = std::stoull(parts[off_idx]);
        size_t size = std::stoull(parts[size_idx]);
        return std::make_tuple(db, offset, size);
    } catch (const std::invalid_argument& e) {
        return std::make_tuple(-1, 0, 0);
    } catch (const std::out_of_range& e) {
        return std::make_tuple(-1, 0, 0);
    }
}

/**
 * GET /memory/db/<db>/offset/<off>/size/<sz>
 *
 * Reads raw bytes from a single database. Response is raw bytes (binary/octet-stream),
 * not JSON — this enforces the constraint that single-DB operations work with direct
 * C++ struct semantics compatible with S7 memory layout.
 */
void HttpAdapter::handleGetMemoryBinary(const httplib::Request& t_req, httplib::Response& t_res, PlcMemory& t_memory) {
    // Extract path: req.matches[1] = the full matched path after /memory/
    std::string t_path = t_req.matches[1];
    if (!t_path.empty() && t_path.back() == '/')
        t_path.pop_back();

    auto [db_num, offset, size] = parseMemoryPath(t_path);

    if (db_num < 0 || size == 0) {
        t_res.status = 400;
        t_res.set_content(R"({"error":"Invalid t_path format: /t_memory/db/<db>/offset/<off>/size/<sz>"})", "application/json");
        return;
    }

    // IP ACL check
    const std::string& client_ip = t_req.remote_addr;
    if (!isAuthorized(t_req, db_num)) {
        t_res.status = 403;
        t_res.set_content(fmt::format(R"({{"error":"IP {} is not authorised to read DB{}"}})", client_ip, db_num), "application/json");
        return;
    }

    // Read from PLC memory
    std::vector<uint8_t> buffer(size, 0);
    if (auto r = t_memory.readDbMemory(static_cast<uint16_t>(db_num), offset, size, buffer.data()); !r) {
        t_res.status = 413;
        t_res.set_content(fmt::format(R"({{"error":"{}"}})", r.error()), "application/json");
        return;
    }

    // Return raw bytes (no JSON wrapping)
    t_res.set_content(reinterpret_cast<const char*>(buffer.data()), size, "application/octet-stream");
}

/**
 * PUT /memory/db/<db>/offset/<off>/size/<sz>
 *
 * Writes raw bytes to a single database. Request body must be exactly <sz> bytes
 * (no padding, no truncation). Response echoes the written bytes (S7 semantics).
 */
void HttpAdapter::handlePutMemoryBinary(const httplib::Request& t_req, httplib::Response& t_res, PlcMemory& t_memory) {
    std::string t_path = t_req.matches[1];
    if (!t_path.empty() && t_path.back() == '/')
        t_path.pop_back();

    auto [db_num, offset, size] = parseMemoryPath(t_path);

    if (db_num < 0 || size == 0) {
        t_res.status = 400;
        t_res.set_content(R"({"error":"Invalid t_path format: /t_memory/db/<db>/offset/<off>/size/<sz>"})", "application/json");
        return;
    }

    // Validate body size
    if (t_req.body.size() != size) {
        t_res.status = 400;
        t_res.set_content(
            fmt::format(R"({{"error":"Body size {} does not match declared size {}"}})", t_req.body.size(), size), "application/json");
        return;
    }

    // IP ACL check
    const std::string& client_ip = t_req.remote_addr;
    if (!isAuthorized(t_req, db_num)) {
        t_res.status = 403;
        t_res.set_content(fmt::format(R"({{"error":"IP {} is not authorised to write DB{}"}})", client_ip, db_num), "application/json");
        return;
    }

    // Write to PLC memory
    const uint8_t* p_payload = reinterpret_cast<const uint8_t*>(t_req.body.data());
    if (auto r = t_memory.writeDbMemory(static_cast<uint16_t>(db_num), offset, size, p_payload); !r) {
        t_res.status = 413;
        t_res.set_content(fmt::format(R"({{"error":"{}"}})", r.error()), "application/json");
        return;
    }

    // Echo written bytes (S7 semantics: confirm write)
    t_res.set_content(reinterpret_cast<const char*>(p_payload), size, "application/octet-stream");
}

/**
 * POST /memory/batch (read)
 *
 * Reads from multiple databases in a single atomic batch. Query parameters specify
 * (db, offset, size) triplets in repeating pattern. Response is JSON array of
 * base64url-encoded items.
 *
 * Request can be empty (uses query params) or a hint JSON that is ignored.
 * Query format: ?db=<n1>&offset=<o1>&size=<s1>&db=<n2>&offset=<o2>&size=<s2>&...
 */
/**
 * PUT /memory/batch
 *
 * Writes to multiple databases in a single atomic batch operation.
 * Request body is JSON array of {db, offset, size, data: base64url} items.
 * All-or-nothing semantics: if any span fails, entire batch is rolled back.
 *
 * Uses PlcMemory batch span API for efficient locking:
 *   - Collects all spans, groups by unique DB
 *   - Acquires one lock per unique DB (ordered by arena offset to prevent deadlock)
 *   - Executes all writes within locks
 *   - Releases locks
 * This dramatically reduces lock overhead vs. per-item writes.
 */
void HttpAdapter::handlePutMemoryBatch(const httplib::Request& t_req, httplib::Response& t_res, PlcMemory& t_memory) {
    // Parse JSON request body
    rapidjson::Document doc;
    doc.Parse(t_req.body.c_str());

    if (doc.HasParseError() || !doc.IsArray()) {
        t_res.status = 400;
        t_res.set_content(R"({"error":"PUT /t_memory/batch requires JSON array body"})", "application/json");
        return;
    }

    // First pass: validate and collect all items + ACL checks
    std::vector<twin::DbMemorySpan> write_spans;
    std::vector<std::vector<uint8_t>> payloads;

    for (rapidjson::SizeType i = 0; i < doc.Size(); ++i) {
        const auto& item = doc[i];

        if (!item.IsObject() || !item.HasMember("db") || !item["db"].IsUint() || !item.HasMember("offset") || !item["offset"].IsUint64() ||
            !item.HasMember("size") || !item["size"].IsUint64() || !item.HasMember("data") || !item["data"].IsString()) {
            t_res.status = 400;
            t_res.set_content(
                R"JSON({"error":"Each item must have db (uint), offset (uint64), size (uint64), and data (string/base64url)"})JSON",
                "application/json");
            return;
        }

        uint16_t db_num = static_cast<uint16_t>(item["db"].GetUint());
        size_t item_offset = item["offset"].GetUint64();
        size_t item_size = item["size"].GetUint64();
        std::string data_b64 = item["data"].GetString();

        if (item_size == 0) {
            t_res.status = 400;
            t_res.set_content(R"({"error":"size must be > 0"})", "application/json");
            return;
        }

        // IP ACL check
        const std::string& client_ip = t_req.remote_addr;
        if (!isAuthorized(t_req, db_num)) {
            t_res.status = 403;
            t_res.set_content(fmt::format(R"({{"error":"IP {} is not authorised to write DB{}"}})", client_ip, db_num), "application/json");
            return;
        }

        // Decode base64url
        std::vector<uint8_t> p_payload = sgrn::utils::encoding::fromBase64(data_b64);
        if (p_payload.size() < item_size) {
            t_res.status = 400;
            t_res.set_content(
                fmt::format(R"({{"error":"Base64url decodes to {} bytes but size={} was requested"}})", p_payload.size(), item_size),
                "application/json");
            return;
        }
        p_payload.resize(item_size);

        // Collect for batch write
        payloads.push_back(std::move(p_payload));
        write_spans.push_back({.db = db_num, .offset = item_offset, .size = item_size, .p_buffer = payloads.back().data()});
    }

    // Second pass: atomic batch write (all or nothing)
    if (auto r = t_memory.writeDbMemory(std::span(write_spans)); !r) {
        t_res.status = 413;
        t_res.set_content(
            fmt::format(R"json({{"error":"{}","items":{},"reason":"atomic write failed, no items written"}})json", r.error(), doc.Size()),
            "application/json");
        return;
    }

    // Third pass: build success response
    rapidjson::Document response;
    response.SetArray();
    auto& alloc = response.GetAllocator();

    for (size_t i = 0; i < write_spans.size(); ++i) {
        rapidjson::Value res_item(rapidjson::kObjectType);
        res_item.AddMember("db", write_spans[i].db, alloc);
        res_item.AddMember("offset", static_cast<uint64_t>(write_spans[i].offset), alloc);
        res_item.AddMember("size", static_cast<uint64_t>(write_spans[i].size), alloc);

        std::string written_b64 = sgrn::utils::encoding::toBase64Url(payloads[i].data(), payloads[i].size());
        res_item.AddMember("written", rapidjson::Value(written_b64.c_str(), alloc), alloc);

        response.PushBack(res_item, alloc);
    }

    t_res.status = 200;
    t_res.set_content(sgrn::utils::json::serializeCompact(response), "application/json");
}
} // namespace sgrn::gateway::adapters
