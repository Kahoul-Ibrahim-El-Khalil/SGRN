#include <fmt/format.h>
#include <sgrn/debug.hpp>
#include <sgrn/gateway/adapters/s7/PlcClient.hpp>
#include <sgrn/gateway/twin/utils.hpp>
#include <sgrn/gateway/wrappers/s7/ProtocolError.hpp>
#include <sgrn/gateway/wrappers/s7/S7Client.hpp>
#include <sgrn/s7shell/PlcTagTable.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <rapidjson/document.h>

namespace sgrn::s7shell
{
using sgrn::gateway::twin::DbField;
using sgrn::gateway::twin::decodeFieldAt;
using sgrn::gateway::twin::encodeFieldAt;
using ::sgrn::scl::ErrorCode;
using ::sgrn::scl::SchemaCode;
using S7Error = ::sgrn::gateway::wrappers::s7::S7Error;
using S7ProtocolCode = ::sgrn::gateway::wrappers::s7::S7ProtocolCode;

// Bridge function to translate domain-specific compilation/validation errors (sgrn::scl::Error)
// into lower-level protocol errors (S7Error) with an 'Unknown' categorization.
static S7Error scl_bridge(const ::sgrn::scl::Error& t_e) noexcept {
    S7ProtocolCode code;
    switch (t_e.code()) {
        case SchemaCode::NotFound:
        case SchemaCode::InvalidType:
        case SchemaCode::OutOfRange:
        case SchemaCode::ParseError:
            code = S7ProtocolCode::InvalidParam;
            break;
        case SchemaCode::Conflict:
        case SchemaCode::SerializationError:
        case SchemaCode::OptimizedAccess:
        case SchemaCode::Generic:
            code = S7ProtocolCode::Unknown;
            break;
    }
    if (code == S7ProtocolCode::Unknown) {
        SGRN_ERROR_LOG("PlcTagTable", "scl_bridge translation fallback: {}", t_e.string());
    }
    return S7Error{code};
}

namespace
{
// Snap7 protocol area and word length constants
#include <snap7.h>
} // namespace

PlcTagTable::PlcTagTable(const std::string& t_registry_path) {
    auto res = loadRegistry_(t_registry_path);
    if (res.hasError())
        fmt::print(stderr, "[PlcTagTable] failed to load registry '{}': {}\n", t_registry_path, res.error().string());
}

// loadRegistry_ parses the JSON tag registry file to register symbolic tags.
// Each tag is assigned a specific S7 area, offset, data type, and size.
sgrn::Result<void, ::sgrn::scl::Error> PlcTagTable::loadRegistry_(const std::string& t_path) {
    std::ifstream f(t_path);
    if (!f.is_open())
        return ::sgrn::scl::Err::Generic("[PlcTagTable] cannot open registry file '{}'", t_path);
    std::string json_str((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    rapidjson::Document root;
    root.Parse(json_str.c_str());
    if (root.HasParseError())
        return ::sgrn::scl::Err::InvalidType("[PlcTagTable] JSON parse error in '{}' at offset {}", t_path, root.GetErrorOffset());

    // Iterate through members of the root JSON object (one member per tag)
    for (auto it = root.MemberBegin(); it != root.MemberEnd(); ++it) {
        const std::string t_name = it->name.GetString();
        const auto& cfg = it->value;
        if (!cfg.IsObject())
            continue;

        auto blk = std::make_unique<Block>();
        blk->name = t_name;
        blk->type_name = cfg.HasMember("type") ? cfg["type"].GetString() : "Real";
        if (auto t = sgrn::scl::parseS7Type(blk->type_name)) {
            blk->type = *t;
        }

        // Map textual S7 areas (e.g., "DB", "MK", "PA", "PE") to protocol codes
        std::string area_str = cfg.HasMember("area") ? cfg["area"].GetString() : "DB";
        if (area_str == "DB")
            blk->addr.area = S7AreaDB;
        else if (area_str == "MK")
            blk->addr.area = S7AreaMK;
        else if (area_str == "PA")
            blk->addr.area = S7AreaPA;
        else if (area_str == "PE")
            blk->addr.area = S7AreaPE;
        else {
            fmt::print(stderr, "[PlcTagTable] unknown area '{}' for tag '{}', skipping\n", area_str, t_name);
            continue;
        }

        // Retrieve offsets and sizes within the mapped area
        blk->addr.db_number = cfg.HasMember("db") ? cfg["db"].GetInt() : 0;
        blk->addr.start_byte = cfg.HasMember("start") ? cfg["start"].GetInt() : 0;
        blk->addr.bit_offset = cfg.HasMember("bit") ? cfg["bit"].GetInt() : 0;

        // Establish protocol word lengths and byte lengths based on the tag type
        if (blk->type_name == "Bool") {
            blk->addr.word_len = S7WLBit;
            blk->addr.byte_count = 1;
        } else if (blk->type_name == "Byte") {
            blk->addr.word_len = S7WLByte;
            blk->addr.byte_count = 1;
        } else if (blk->type_name == "Int" || blk->type_name == "Word") {
            blk->addr.word_len = S7WLWord;
            blk->addr.byte_count = 2;
        } else if (blk->type_name == "DInt" || blk->type_name == "DWord") {
            blk->addr.word_len = S7WLDWord;
            blk->addr.byte_count = 4;
        } else if (blk->type_name == "Real") {
            blk->addr.word_len = S7WLReal;
            blk->addr.byte_count = 4;
        }

        // Initialize local shadow cache state
        blk->raw_data.assign(blk->addr.byte_count, 0u);
        blk->json_cache = "null";

        // Capture baseline state to facilitate clean/dirty tracking comparisons
        blk->tracker.captureBaseline(std::vector<uint8_t>(blk->addr.byte_count, 0));

        // Synchronize with the base class TagTable configuration so core/web gateway features
        // can query these tags in metadata structures
        ::sgrn::Tag core_tag;
        core_tag.name = t_name;
        core_tag.type_name = blk->type_name;
        core_tag.size = blk->addr.byte_count;
        core_tag.address_context["area"] = blk->addr.area;
        core_tag.address_context["db"] = blk->addr.db_number;
        core_tag.address_context["start"] = blk->addr.start_byte;
        core_tag.address_context["bit"] = blk->addr.bit_offset;
        ::sgrn::TagTable::addTag(std::move(core_tag));

        blocks_.emplace(t_name, std::move(blk));
    }
    return {};
}

size_t PlcTagTable::tagCount() const noexcept {
    return blocks_.size();
}

sgrn::Result<PlcTagTable::TagDescriptor, ::sgrn::scl::Error> PlcTagTable::describeTag(const std::string& t_name) const {
    const sgrn::Result<const Block*, ::sgrn::scl::Error> blk = findBlock_(t_name);
    if (blk.hasError())
        return std::unexpected(blk.error());
    TagDescriptor d;
    d.name = blk.value()->name;
    d.addr = blk.value()->addr;
    d.type_name = blk.value()->type_name;
    d.type = blk.value()->type;
    return d;
}

std::vector<std::string> PlcTagTable::tagNames() const {
    std::vector<std::string> names;
    names.reserve(blocks_.size());
    for (const auto& [key, _] : blocks_)
        names.push_back(key);
    std::sort(names.begin(), names.end());
    return names;
}

// read reads the value of a tag from the local shadow memory cache (no network activity).
// Returns the JSON string cached during the last PLC read or local write.
sgrn::Result<std::string, S7Error> PlcTagTable::read(const std::string& t_path) const {
    const sgrn::Result<const Block*, ::sgrn::scl::Error> blk = findBlock_(t_path);
    if (blk.hasError())
        return std::unexpected(scl_bridge(blk.error()));
    std::lock_guard<std::mutex> lk(blk.value()->mu);
    return blk.value()->json_cache;
}

// write stages a new value into the local shadow memory cache (no network activity).
// The value is encoded from JSON to raw binary bytes, copied to the block buffer,
// and the tag is marked dirty so it gets pushed to the PLC during the next synchronization cycle.
sgrn::Result<void, S7Error> PlcTagTable::write(const std::string& t_path, const std::string& t_json_val) {
    const sgrn::Result<Block*, ::sgrn::scl::Error> blk = findBlock_(t_path);
    if (blk.hasError())
        return std::unexpected(scl_bridge(blk.error()));

    sgrn::Result<std::vector<uint8_t>, ::sgrn::scl::Error> encoded = encodeValue_(*blk.value(), t_json_val);
    if (encoded.hasError())
        return std::unexpected(scl_bridge(encoded.error()));

    std::lock_guard<std::mutex> lk(blk.value()->mu);
    std::memcpy(blk.value()->raw_data.data(), encoded.value().data(), encoded.value().size());
    blk.value()->json_cache = t_json_val;
    blk.value()->tracker.markDirty(); // Flags this block for updates on the network
    return {};
}

// get performs a synchronous single-var read from the PLC over the network.
// It retrieves the raw S7 memory bytes, updates the shadow cache, decodes the raw value
// back into JSON format, resets the dirty flag, and returns the JSON string representation.
sgrn::Result<std::string, S7Error> PlcTagTable::get(S7Client& t_client, const std::string& t_path) {
    const sgrn::Result<Block*, ::sgrn::scl::Error> blk = findBlock_(t_path);
    if (blk.hasError())
        return std::unexpected(scl_bridge(blk.error()));

    S7DataItem t_item{};
    std::vector<uint8_t> t_buf;
    fillGetItem_(*blk.value(), t_item, t_buf);

    auto rc = t_client.readMultiVars(&t_item, 1);
    if (rc.hasError())
        return std::unexpected(S7Error{S7ProtocolCode::ReadError});
    if (t_item.Result != 0)
        return std::unexpected(S7Error{S7ProtocolCode::ReadError});

    // commitRaw_ decodes the bytes from the PLC and populates json_cache
    auto commit_res = commitRaw_(*blk.value(), t_item.pdata, blk.value()->addr.byte_count);
    if (commit_res.hasError())
        return std::unexpected(scl_bridge(commit_res.error()));

    std::lock_guard<std::mutex> lk(blk.value()->mu);
    return blk.value()->json_cache;
}

// put performs a synchronous single-var write to the PLC over the network.
// It encodes the provided JSON value into S7 raw binary format, transmits it to the PLC,
// updates the local shadow memory buffer, and marks the block clean.
sgrn::Result<void, S7Error> PlcTagTable::put(S7Client& t_client, const std::string& t_path, const std::string& t_json_val) {
    const sgrn::Result<Block*, ::sgrn::scl::Error> blk = findBlock_(t_path);
    if (blk.hasError())
        return std::unexpected(scl_bridge(blk.error()));

    sgrn::Result<std::vector<uint8_t>, ::sgrn::scl::Error> t_buf = encodeValue_(*blk.value(), t_json_val);
    if (t_buf.hasError())
        return std::unexpected(scl_bridge(t_buf.error()));

    S7DataItem t_item{};
    t_item.Area = blk.value()->addr.area;
    t_item.WordLen = blk.value()->addr.word_len;
    t_item.DBNumber = blk.value()->addr.db_number;
    // Calculate S7 bit offset for Bools: StartByte * 8 + BitOffset
    t_item.Start = (blk.value()->addr.word_len == S7WLBit) ? blk.value()->addr.start_byte * 8 + blk.value()->addr.bit_offset
                                                           : blk.value()->addr.start_byte;
    t_item.Amount = (blk.value()->addr.word_len == S7WLBit) ? 1 : blk.value()->addr.byte_count;
    t_item.pdata = t_buf.value().data();

    auto rc = t_client.writeMultiVars(&t_item, 1);
    if (rc.hasError())
        return std::unexpected(S7Error{S7ProtocolCode::WriteError});
    if (t_item.Result != 0)
        return S7Error{S7ProtocolCode::WriteError};

    std::lock_guard<std::mutex> lk(blk.value()->mu);
    std::memcpy(blk.value()->raw_data.data(), t_buf.value().data(), t_buf.value().size());
    blk.value()->json_cache = t_json_val;
    blk.value()->tracker.markClean(); // Mark clean since the PLC matches our local cache
    return {};
}

sgrn::Result<void, S7Error> PlcTagTable::buildGetItems(
    const std::vector<std::string>& t_paths, std::vector<S7DataItem>& t_out_items, std::vector<std::vector<uint8_t>>& t_out_bufs) {
    for (const auto& t_path : t_paths) {
        const sgrn::Result<Block*, ::sgrn::scl::Error> blk = findBlock_(t_path);
        if (blk.hasError())
            return scl_bridge(blk.error());
        t_out_bufs.emplace_back();
        t_out_items.emplace_back();
        fillGetItem_(*blk.value(), t_out_items.back(), t_out_bufs.back());
    }
    return {};
}

sgrn::Result<void, S7Error> PlcTagTable::buildPutItems(const std::vector<std::string>& t_paths,
    const std::vector<std::variant<std::string, s7codec::DecodedValue>>& t_values, std::vector<S7DataItem>& t_out_items,
    std::vector<std::vector<uint8_t>>& t_out_bufs) {
    for (size_t i = 0; i < t_paths.size(); ++i) {
        const sgrn::Result<Block*, ::sgrn::scl::Error> blk = findBlock_(t_paths[i]);
        if (blk.hasError())
            return std::unexpected(scl_bridge(blk.error()));

        if (std::holds_alternative<std::string>(t_values[i])) {
            sgrn::Result<std::vector<uint8_t>, ::sgrn::scl::Error> encoded = encodeValue_(*blk.value(), std::get<std::string>(t_values[i]));
            if (encoded.hasError())
                return std::unexpected(scl_bridge(encoded.error()));
            t_out_bufs.push_back(std::move(encoded.value()));
        } else {
            const auto& dv = std::get<s7codec::DecodedValue>(t_values[i]);
            DbField fd;
            fd.type = blk.value()->type;
            // For variable-length string types, count encodes the max length:
            //   String  : max_len = byte_count - 2  (2-byte header)
            //   WString : max_len = (byte_count - 4) / 2  (4-byte header, 2 bytes/char)
            //   XString : max_len = byte_count - 8  (8-byte header)
            //   XWString: max_len = (byte_count - 8) / 2
            // For all other types, count = 1 (single element encode).
            if (blk.value()->type == s7codec::Type::String)
                fd.count = std::max(0, blk.value()->addr.byte_count - 2);
            else if (blk.value()->type == s7codec::Type::WString)
                fd.count = std::max(0, (blk.value()->addr.byte_count - 4) / 2);
            else if (blk.value()->type == s7codec::Type::XString)
                fd.count = std::max(0, blk.value()->addr.byte_count - 8);
            else if (blk.value()->type == s7codec::Type::XWString)
                fd.count = std::max(0, (blk.value()->addr.byte_count - 8) / 2);
            else
                fd.count = 1;
            std::vector<uint8_t> t_buf(blk.value()->addr.byte_count, 0u);

            int bit_idx = (blk.value()->addr.word_len == S7WLBit) ? blk.value()->addr.bit_offset : 0;
            auto status = s7codec::encodeScalar(dv, fd.type, t_buf.data(), t_buf.size(), bit_idx, fd.count);
            if (!status.ok())
                return S7Error{S7ProtocolCode::Unknown};
            t_out_bufs.push_back(std::move(t_buf));
        }

        S7DataItem t_item{};
        t_item.Area = blk.value()->addr.area;
        t_item.WordLen = blk.value()->addr.word_len;
        t_item.DBNumber = blk.value()->addr.db_number;
        t_item.Start = (blk.value()->addr.word_len == S7WLBit) ? blk.value()->addr.start_byte * 8 + blk.value()->addr.bit_offset
                                                               : blk.value()->addr.start_byte;
        t_item.Amount = (blk.value()->addr.word_len == S7WLBit) ? 1 : blk.value()->addr.byte_count;
        t_item.pdata = t_out_bufs.back().data();
        t_out_items.push_back(t_item);
    }
    return {};
}

sgrn::Result<void, S7Error> PlcTagTable::commitLocalPut(
    const std::vector<std::string>& t_paths, const std::vector<std::variant<std::string, s7codec::DecodedValue>>& t_values) {
    for (size_t i = 0; i < t_paths.size(); ++i) {
        const sgrn::Result<Block*, ::sgrn::scl::Error> blk = findBlock_(t_paths[i]);
        if (blk.hasError())
            return std::unexpected(scl_bridge(blk.error()));

        std::vector<uint8_t> encoded;
        std::string t_json_val;

        if (std::holds_alternative<std::string>(t_values[i])) {
            t_json_val = std::get<std::string>(t_values[i]);
            auto enc_res = encodeValue_(*blk.value(), t_json_val);
            if (enc_res.hasError())
                return std::unexpected(scl_bridge(enc_res.error()));
            encoded = std::move(enc_res.value());
        } else {
            const auto& dv = std::get<s7codec::DecodedValue>(t_values[i]);
            t_json_val = s7codec::formatDecodedValue(dv, blk.value()->type);

            DbField fd;
            fd.type = blk.value()->type;
            // Derive max_len from byte count — same logic as buildPutItems.
            if (blk.value()->type == s7codec::Type::String)
                fd.count = std::max(0, blk.value()->addr.byte_count - 2);
            else if (blk.value()->type == s7codec::Type::WString)
                fd.count = std::max(0, (blk.value()->addr.byte_count - 4) / 2);
            else if (blk.value()->type == s7codec::Type::XString)
                fd.count = std::max(0, blk.value()->addr.byte_count - 8);
            else if (blk.value()->type == s7codec::Type::XWString)
                fd.count = std::max(0, (blk.value()->addr.byte_count - 8) / 2);
            else
                fd.count = 1;
            encoded.assign(blk.value()->addr.byte_count, 0u);

            int bit_idx = (blk.value()->addr.word_len == S7WLBit) ? blk.value()->addr.bit_offset : 0;
            auto status = s7codec::encodeScalar(dv, fd.type, encoded.data(), encoded.size(), bit_idx, fd.count);
            if (!status.ok())
                return S7Error{S7ProtocolCode::Unknown};
        }

        std::lock_guard<std::mutex> lk(blk.value()->mu);
        std::memcpy(blk.value()->raw_data.data(), encoded.data(), encoded.size());
        blk.value()->json_cache = std::move(t_json_val);
        blk.value()->tracker.markClean();
    }
    return {};
}

void PlcTagTable::commitGetResults(const std::vector<std::string>& t_paths, const std::vector<S7DataItem>& t_items) {
    for (size_t i = 0; i < t_paths.size(); ++i) {
        if (t_items[i].Result != 0)
            continue;
        const sgrn::Result<Block*, ::sgrn::scl::Error> blk = findBlock_(t_paths[i]);
        if (blk.hasError())
            continue;
        (void)commitRaw_(*blk.value(), t_items[i].pdata, blk.value()->addr.byte_count);
    }
}

// pullAll reads all registered tags from the PLC in one go.
// It creates structured S7DataItems and chunks network requests into groups of 19 (the Snap7 PDU limit).
sgrn::Result<void, ::sgrn::scl::Error> PlcTagTable::pullAll(S7Client& t_client) {
    std::vector<std::string> all_paths;
    for (const auto& [t_name, _] : blocks_)
        all_paths.push_back(t_name);

    std::vector<S7DataItem> t_items;
    std::vector<std::vector<uint8_t>> bufs;
    auto build_res = buildGetItems(all_paths, t_items, bufs);
    if (build_res.hasError())
        return ::sgrn::scl::Err::Generic("PlcTagTable: buildGetItems failed: {}", build_res.error().string());

    // Chunk size is limited to 19 to fit the maximum variable items in a single S7 PDU.
    size_t offset = 0;
    while (offset < t_items.size()) {
        const size_t count = std::min(static_cast<size_t>(19), t_items.size() - offset);
        auto rc = t_client.readMultiVars(t_items.data() + offset, static_cast<int>(count));
        if (rc.hasError())
            return ::sgrn::scl::Err::Generic("PlcTagTable: pullAll chunk failed: {}", rc.error().string());
        offset += count;
    }
    commitGetResults(all_paths, t_items);
    return {};
}

// pushDirty writes all locally modified (dirty) tags back to the PLC.
// If a write fails at the transaction level, it rolls back by re-marking modified tags
// as dirty so they are not silently skipped in subsequent push cycles.
sgrn::Result<void, ::sgrn::scl::Error> PlcTagTable::pushDirty(S7Client& t_client) {
    std::vector<std::string> dirty_paths;
    std::vector<std::variant<std::string, s7codec::DecodedValue>> dirty_values;

    // Scan blocks for dirty markers under mutex protection.
    // getAndClearDirty() atomically checks the flag and resets it.
    for (const auto& [t_name, blk] : blocks_) {
        std::lock_guard<std::mutex> lk(blk->mu);
        if (!blk->tracker.getAndClearDirty())
            continue;
        dirty_paths.push_back(t_name);
        dirty_values.push_back(blk->json_cache);
    }
    if (dirty_paths.empty())
        return {};

    std::vector<S7DataItem> t_items;
    std::vector<std::vector<uint8_t>> bufs;
    auto build_res = buildPutItems(dirty_paths, dirty_values, t_items, bufs);
    if (build_res.hasError()) {
        // Rollback: Re-mark blocks as dirty if compilation/encoding fails
        for (const auto& t_name : dirty_paths) {
            const auto blk = findBlock_(t_name);
            if (!blk.hasError()) {
                std::lock_guard<std::mutex> lk(blk.value()->mu);
                blk.value()->tracker.markDirty();
            }
        }
        return ::sgrn::scl::Err::Generic("PlcTagTable: buildPutItems failed: {}", build_res.error().string());
    }

    // Write to the PLC in transactions of 19 items max
    size_t offset = 0;
    while (offset < t_items.size()) {
        const size_t count = std::min(static_cast<size_t>(19), t_items.size() - offset);
        auto rc = t_client.writeMultiVars(t_items.data() + offset, static_cast<int>(count));
        if (rc.hasError()) {
            // Rollback on network failure
            for (const auto& t_name : dirty_paths) {
                const auto blk = findBlock_(t_name);
                if (!blk.hasError()) {
                    std::lock_guard<std::mutex> lk(blk.value()->mu);
                    blk.value()->tracker.markDirty();
                }
            }
            return ::sgrn::scl::Err::Generic("PlcTagTable: pushDirty chunk failed: {}", rc.error().string());
        }
        offset += count;
    }

    // Commit results: mark clean only if the individual item write succeeded at the PLC.
    for (size_t i = 0; i < dirty_paths.size(); ++i) {
        const sgrn::Result<Block*, ::sgrn::scl::Error> blk = findBlock_(dirty_paths[i]);
        if (!blk.hasError()) {
            std::lock_guard<std::mutex> lk(blk.value()->mu);
            if (t_items[i].Result != 0) {
                blk.value()->tracker.markDirty(); // Keep dirty if PLC rejected this item specifically
            } else {
                blk.value()->tracker.markClean();
            }
        }
    }
    return {};
}

sgrn::Result<PlcTagTable::Block*, ::sgrn::scl::Error> PlcTagTable::findBlock_(const std::string& t_path) {
    auto it = blocks_.find(t_path);
    if (it == blocks_.end())
        return ::sgrn::scl::Err::NotFound("PlcTagTable: unknown tag '{}'", t_path);
    return it->second.get();
}

sgrn::Result<const PlcTagTable::Block*, ::sgrn::scl::Error> PlcTagTable::findBlock_(const std::string& t_path) const {
    auto it = blocks_.find(t_path);
    if (it == blocks_.end())
        return ::sgrn::scl::Err::NotFound("PlcTagTable: unknown tag '{}'", t_path);
    return it->second.get();
}

void PlcTagTable::fillGetItem_(const Block& t_b, S7DataItem& t_item, std::vector<uint8_t>& t_buf) const {
    t_buf.assign(t_b.addr.byte_count, 0u);
    t_item.Area = t_b.addr.area;
    t_item.WordLen = t_b.addr.word_len;
    t_item.DBNumber = t_b.addr.db_number;
    t_item.Start = (t_b.addr.word_len == S7WLBit) ? t_b.addr.start_byte * 8 + t_b.addr.bit_offset : t_b.addr.start_byte;
    t_item.Amount = (t_b.addr.word_len == S7WLBit) ? 1 : t_b.addr.byte_count;
    t_item.pdata = t_buf.data();
}

sgrn::Result<std::vector<uint8_t>, ::sgrn::scl::Error> PlcTagTable::encodeValue_(const Block& t_b, const std::string& t_json_val) const {
    DbField fd;
    fd.type = t_b.type;
    fd.count = 1;
    std::vector<uint8_t> t_buf(t_b.addr.byte_count, 0);
    auto res = ::sgrn::gateway::twin::encodeFieldAt(fd, t_json_val, t_buf.data(), t_b.addr.byte_count);
    if (res.hasError())
        return ::sgrn::scl::Err::InvalidType("PlcTagTable: encode failed: {}", res.error().string());
    return t_buf;
}

sgrn::Result<std::string, ::sgrn::scl::Error> PlcTagTable::decodeRaw_(const Block& t_b) const {
    DbField fd;
    fd.type = t_b.type;
    fd.count = 1;
    return ::sgrn::gateway::twin::decodeFieldAt(fd, t_b.raw_data.data(), t_b.raw_data.size());
}

sgrn::Result<void, ::sgrn::scl::Error> PlcTagTable::commitRaw_(Block& t_b, const void* tp_data, size_t t_size) {
    std::lock_guard<std::mutex> lk(t_b.mu);
    const auto* p_bytes = static_cast<const uint8_t*>(tp_data);
    t_b.tracker.checkDirty(t_b.raw_data.data(), p_bytes, t_size);
    std::memcpy(t_b.raw_data.data(), p_bytes, t_size);
    auto decoded = decodeRaw_(t_b);
    if (decoded.hasError())
        return std::unexpected(decoded.error());
    t_b.json_cache = std::move(decoded.value());
    t_b.tracker.captureBaseline(t_b.raw_data.data(), t_size);
    return {};
}

void PlcTagTable::addTag(const ::sgrn::scl::PlcTag& t_tag) {
    auto blk = std::make_unique<Block>();
    blk->name = t_tag.name;
    blk->type_name = t_tag.type_str;
    blk->type = t_tag.type;

    blk->addr.area = t_tag.addr.area;
    blk->addr.db_number = t_tag.addr.db_number;
    blk->addr.start_byte = t_tag.addr.byte_offset;
    blk->addr.bit_offset = t_tag.addr.bit_index;
    blk->addr.word_len = t_tag.addr.word_len;
    blk->addr.byte_count = t_tag.addr.byte_count;

    blk->raw_data.assign(blk->addr.byte_count, 0u);
    blk->json_cache = "null";
    blk->tracker.captureBaseline(std::vector<uint8_t>(blk->addr.byte_count, 0));

    ::sgrn::Tag core_tag;
    core_tag.name = t_tag.name;
    core_tag.type_name = t_tag.type_str;
    core_tag.size = t_tag.addr.byte_count;
    core_tag.address_context["area"] = t_tag.addr.area;
    core_tag.address_context["db"] = t_tag.addr.db_number;
    core_tag.address_context["start"] = t_tag.addr.byte_offset;
    core_tag.address_context["bit"] = t_tag.addr.bit_index;

    ::sgrn::TagTable::addTag(std::move(core_tag));
    blocks_.emplace(t_tag.name, std::move(blk));
}

} // namespace sgrn::s7shell