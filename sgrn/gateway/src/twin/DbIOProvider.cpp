#include <fmt/format.h>
#include <sgrn/debug.hpp>
#include <sgrn/gateway/adapters/s7/PlcClient.hpp>
#include <sgrn/gateway/twin/DbIOProvider.hpp>
#include <sgrn/gateway/twin/DbIoError.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/utils.hpp>
#include <sgrn/gateway/wrappers/s7/error.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <snap7.h>

namespace sgrn::gateway::twin
{
namespace scl = ::sgrn::scl;
using ::sgrn::gateway::adapters::s7::PlcClient;
using ::sgrn::gateway::wrappers::s7::S7DataItem;
using ::sgrn::scl::DataType;
using ::sgrn::scl::DbField;
using ::sgrn::scl::FieldLocation;

DbIOProvider::DbIOProvider(
    PlcMemory& t_memory, PlcSchemaStore& t_schema, uint16_t t_db_num, std::map<uint16_t, DbSnapshot>& t_pending_writes)
    : memory_(t_memory)
    , schema_(t_schema)
    , db_num_(t_db_num)
    , pending_writes_(t_pending_writes) {
}

// read retrieves a field's value directly from the local shadow memory cache (no network).
sgrn::Result<std::string, DbIoError> DbIOProvider::read(const std::string& t_path) const {
    auto raw = memory_.getFieldValue(db_num_, t_path);
    if (raw.hasError())
        return std::unexpected(fromPlcMemoryErrorToDbIoError(raw.error()));

    return std::move(raw).value();
}

// write stages a field's value directly in the local shadow memory cache (no network).
sgrn::Result<void, DbIoError> DbIOProvider::write(const std::string& t_path, const std::string& t_json_val) {

    const auto loc = resolveField(t_path);
    SGRN_RETURN_IF(loc.hasError(), loc.error());

    const auto raw = memory_.updateField(db_num_, t_path, t_json_val);
    SGRN_RETURN_IF(raw.hasError(), fromPlcMemoryErrorToDbIoError(raw.error()));
    return {};
}

// get performs an immediate single-field synchronous network read from the PLC,
// updates the shadow cache memory, and returns the decoded string value.
sgrn::Result<std::string, DbIoError> DbIOProvider::get(S7Client& t_client, const std::string& t_path) {

    SGRN_RETURN_IF(!t_client.isConnected(), DbIoError::NotConnected);

    const sgrn::Result<scl::FieldLocation, DbIoError> t_loc = resolveField(t_path);

    SGRN_RETURN_IF(t_loc.hasError(), t_loc.error());

    S7DataItem t_item{};
    std::vector<uint8_t> buf;
    buildOneGetItem(t_loc.value(), t_item, buf);

    auto rc = t_client.readMultiVars(&t_item, 1);

    SGRN_RETURN_IF(rc.hasError(), fromS7ErrorToDbIoError(rc.error()));

    SGRN_RETURN_IF(t_item.Result != 0, DbIoError::NetworkReadFailed);

    commitOneField(t_path, t_loc.value(), t_item);
    auto raw2 = memory_.getFieldValue(db_num_, t_path);

    SGRN_RETURN_IF(raw2.hasError(), fromPlcMemoryErrorToDbIoError(raw2.error()));
    return std::move(raw2).value();
}

// put (JSON string variant) performs an immediate single-field synchronous network write to the PLC,
// and synchronously updates the local memory cache and parent DB snapshot (ensuring cache consistency).
sgrn::Result<void, DbIoError> DbIOProvider::put(S7Client& t_client, const std::string& t_path, const std::string& t_json_val) {
    SGRN_RETURN_IF(!t_client.isConnected(), DbIoError::NotConnected);

    const sgrn::Result<scl::FieldLocation, DbIoError> t_loc = resolveField(t_path);
    SGRN_RETURN_IF(t_loc.hasError(), t_loc.error());

    sgrn::Result<std::vector<uint8_t>, DbIoError> buf = encodeValue(t_loc.value(), t_json_val);
    SGRN_RETURN_IF(buf.hasError(), buf.error());

    S7DataItem t_item{};
    t_item.Area = S7AreaDB;
    t_item.DBNumber = db_num_;
    if (t_loc.value().field->type == DataType::Bool && t_loc.value().field->count <= 1) {
        t_item.WordLen = S7WLBit;
        t_item.Start = t_loc.value().abs_offset * 8 + t_loc.value().field->bit_index;
        t_item.Amount = 1;
    } else {
        t_item.WordLen = S7WLByte;
        t_item.Start = t_loc.value().abs_offset;
        t_item.Amount = static_cast<int>(buf.value().size());
    }
    t_item.pdata = buf.value().data();

    const auto rc = t_client.writeMultiVars(&t_item, 1);
    SGRN_RETURN_IF(rc.hasError(), fromS7ErrorToDbIoError(rc.error()));
    SGRN_RETURN_IF(t_item.Result != 0, DbIoError::NetworkWriteFailed);

    const auto mem_res = memory_.updateField(db_num_, t_path, t_json_val);
    SGRN_RETURN_IF(mem_res.hasError(), fromPlcMemoryErrorToDbIoError(mem_res.error()));

    // Keep the DB snapshot and shadow cache baseline synchronized so push() doesn't write stale values
    auto it = pending_writes_.find(db_num_);
    if (it != pending_writes_.end()) {
        const auto snap_res = it->second.updateField(t_path, t_json_val);
        SGRN_RETURN_IF(snap_res.hasError(), fromSclErrorToDbIoError(snap_res.error()));
    }
    return {};
}

// put (DecodedValue struct variant) performs an immediate synchronous write to the PLC,
// bypassing the JSON string round trip.
sgrn::Result<void, DbIoError> DbIOProvider::put(S7Client& t_client, const std::string& t_path, const s7codec::DecodedValue& t_val) {
    if (!t_client.isConnected())
        return std::unexpected(DbIoError::NotConnected);

    const sgrn::Result<scl::FieldLocation, DbIoError> t_loc = resolveField(t_path);
    if (t_loc.hasError())
        return std::unexpected(t_loc.error());

    sgrn::Result<std::vector<uint8_t>, DbIoError> buf = encodeValue(t_loc.value(), t_val);
    if (buf.hasError())
        return std::unexpected(buf.error());

    S7DataItem t_item{};
    t_item.Area = S7AreaDB;
    t_item.DBNumber = db_num_;
    if (t_loc.value().field->type == DataType::Bool && t_loc.value().field->count <= 1) {
        t_item.WordLen = S7WLBit;
        t_item.Start = t_loc.value().abs_offset * 8 + t_loc.value().field->bit_index;
        t_item.Amount = 1;
    } else {
        t_item.WordLen = S7WLByte;
        t_item.Start = t_loc.value().abs_offset;
        t_item.Amount = static_cast<int>(buf.value().size());
    }
    t_item.pdata = buf.value().data();

    auto rc = t_client.writeMultiVars(&t_item, 1);

    SGRN_RETURN_ERROR_IF(rc.hasError(), fromS7ErrorToDbIoError(rc.error()));

    SGRN_RETURN_ERROR_IF(t_item.Result != 0, DbIoError::NetworkWriteFailed);

    const std::string t_json_val = s7codec::formatDecodedValue(t_val, t_loc.value().field->type);
    auto mem_res = memory_.updateField(db_num_, t_path, t_json_val);

    SGRN_RETURN_IF(mem_res.hasError(), fromPlcMemoryErrorToDbIoError(mem_res.error()));

    auto it = pending_writes_.find(db_num_);
    if (it != pending_writes_.end()) {
        auto snap_res = it->second.updateField(t_path, t_json_val);
        if (snap_res.hasError())
            return std::unexpected(fromSclErrorToDbIoError(snap_res.error()));
    }
    return {};
}

sgrn::Result<void, DbIoError> DbIOProvider::buildGetItems(
    const std::vector<std::string>& t_paths, std::vector<S7DataItem>& t_out_items, std::vector<std::vector<uint8_t>>& t_out_bufs) {
    for (const auto& t_path : t_paths) {
        const sgrn::Result<scl::FieldLocation, DbIoError> t_loc = resolveField(t_path);
        if (t_loc.hasError())
            return std::unexpected(t_loc.error());
        t_out_bufs.emplace_back();
        t_out_items.emplace_back();
        buildOneGetItem(t_loc.value(), t_out_items.back(), t_out_bufs.back());
    }
    return {};
}

sgrn::Result<void, DbIoError> DbIOProvider::buildPutItems(const std::vector<std::string>& t_paths,
    const std::vector<std::variant<std::string, s7codec::DecodedValue>>& t_values, std::vector<S7DataItem>& t_out_items,
    std::vector<std::vector<uint8_t>>& t_out_bufs) {
    for (size_t i = 0; i < t_paths.size(); ++i) {
        const sgrn::Result<scl::FieldLocation, DbIoError> t_loc = resolveField(t_paths[i]);
        if (t_loc.hasError())
            return std::unexpected(t_loc.error());

        if (std::holds_alternative<std::string>(t_values[i])) {
            sgrn::Result<std::vector<uint8_t>, DbIoError> encoded = encodeValue(t_loc.value(), std::get<std::string>(t_values[i]));
            if (encoded.hasError())
                return std::unexpected(encoded.error());
            t_out_bufs.push_back(std::move(encoded.value()));
        } else {
            sgrn::Result<std::vector<uint8_t>, DbIoError> encoded =
                encodeValue(t_loc.value(), std::get<s7codec::DecodedValue>(t_values[i]));
            if (encoded.hasError())
                return std::unexpected(encoded.error());
            t_out_bufs.push_back(std::move(encoded.value()));
        }

        S7DataItem t_item{};
        t_item.Area = S7AreaDB;
        t_item.DBNumber = db_num_;
        if (t_loc.value().field->type == DataType::Bool && t_loc.value().field->count <= 1) {
            t_item.WordLen = S7WLBit;
            t_item.Start = t_loc.value().abs_offset * 8 + t_loc.value().field->bit_index;
            t_item.Amount = 1;
        } else {
            t_item.WordLen = S7WLByte;
            t_item.Start = t_loc.value().abs_offset;
            t_item.Amount = static_cast<int>(t_out_bufs.back().size());
        }
        t_item.pdata = t_out_bufs.back().data();
        t_out_items.push_back(t_item);
    }
    return {};
}

sgrn::Result<void, DbIoError> DbIOProvider::commitLocalPut(
    const std::vector<std::string>& t_paths, const std::vector<std::variant<std::string, s7codec::DecodedValue>>& t_values) {
    for (size_t i = 0; i < t_paths.size(); ++i) {
        const sgrn::Result<scl::FieldLocation, DbIoError> t_loc = resolveField(t_paths[i]);
        SGRN_RETURN_ERROR_IF(t_loc.hasError(), t_loc.error());

        std::string t_json_val;
        if (std::holds_alternative<std::string>(t_values[i])) {
            t_json_val = std::get<std::string>(t_values[i]);
        } else {
            t_json_val = s7codec::formatDecodedValue(std::get<s7codec::DecodedValue>(t_values[i]), t_loc.value().field->type);
        }

        auto mem_res = memory_.updateField(db_num_, t_paths[i], t_json_val);
        SGRN_RETURN_ERROR_IF(mem_res.hasError(), fromPlcMemoryErrorToDbIoError(mem_res.error()));

        auto it = pending_writes_.find(db_num_);
        if (it != pending_writes_.end()) {
            auto snap_res = it->second.updateField(t_paths[i], t_json_val);
            if (snap_res.hasError()) {
                fmt::print("[DbIOProvider] commitLocalPut error updating snapshot field '{}': {}\n", t_paths[i],
                    ::sgrn::scl::toString(snap_res.error()));
                return fromSclErrorToDbIoError(snap_res.error());
            }
        }
    }
    return {};
}

void DbIOProvider::commitGetResults(const std::vector<std::string>& t_paths, const std::vector<S7DataItem>& t_items) {
    for (size_t i = 0; i < t_paths.size(); ++i) {
        if (t_items[i].Result != 0)
            continue;
        const sgrn::Result<scl::FieldLocation, DbIoError> t_loc = resolveField(t_paths[i]);
        if (t_loc.hasError())
            continue;
        commitOneField(t_paths[i], t_loc.value(), t_items[i]);
    }
}

sgrn::Result<scl::FieldLocation, DbIoError> DbIOProvider::resolveField(const std::string& t_path) const {
    auto t_loc = schema_.findField(db_num_, t_path);
    SGRN_RETURN_IF_NULL(t_loc, DbIoError::FieldNotFound);
    return t_loc.value();
}

void DbIOProvider::buildOneGetItem(const scl::FieldLocation& t_loc, S7DataItem& t_out_item, std::vector<uint8_t>& t_out_buf) const {
    const int sz =
        (t_loc.field->type == DataType::Bool && t_loc.field->count <= 1) ? 1 : ::sgrn::gateway::twin::fieldSpanSize(*t_loc.field);
    t_out_buf.assign(sz, 0u);

    t_out_item.Area = S7AreaDB;
    t_out_item.DBNumber = db_num_;
    if (t_loc.field->type == DataType::Bool && t_loc.field->count <= 1) {
        // Use WLByte instead of WLBit to avoid potential issues with bit-level addressing in some servers
        t_out_item.WordLen = S7WLByte;
        t_out_item.Start = t_loc.abs_offset;
        t_out_item.Amount = 1;
    } else {
        t_out_item.WordLen = S7WLByte;
        t_out_item.Start = t_loc.abs_offset;
        t_out_item.Amount = sz;
    }
    t_out_item.pdata = t_out_buf.data();
}

sgrn::Result<std::vector<uint8_t>, DbIoError> DbIOProvider::encodeValue(
    const scl::FieldLocation& t_loc, const std::string& t_json_val) const {
    const bool is_single_bit = (t_loc.field->type == DataType::Bool && t_loc.field->count <= 1);
    const int sz = is_single_bit ? 1 : ::sgrn::gateway::twin::fieldSpanSize(*t_loc.field);
    std::vector<uint8_t> buf(sz, 0);

    DbField field_to_encode = *t_loc.field;
    auto res = ::sgrn::gateway::twin::encodeFieldAt(field_to_encode, t_json_val, buf.data(), sz, 0, t_loc.field->endianness);

    SGRN_RETURN_ERROR_IF(res.hasError(), fromSclErrorToDbIoError(res.error()));

    return buf;
}

sgrn::Result<std::vector<uint8_t>, DbIoError> DbIOProvider::encodeValue(
    const scl::FieldLocation& t_loc, const s7codec::DecodedValue& t_val) const {
    const bool is_single_bit = (t_loc.field->type == DataType::Bool && t_loc.field->count <= 1);
    const int sz = is_single_bit ? 1 : ::sgrn::gateway::twin::fieldSpanSize(*t_loc.field);
    std::vector<uint8_t> buf(sz, 0);

    int bit_idx = t_loc.field->bit_index;

    auto status = s7codec::encodeScalar(t_val, t_loc.field->type, buf.data(), sz, bit_idx, t_loc.field->count, t_loc.field->endianness);
    SGRN_RETURN_IF(!status.has_value(), DbIoError::EncodeFailed);

    return buf;
}

void DbIOProvider::commitOneField(const std::string& /*path*/, const scl::FieldLocation& t_loc, const S7DataItem& t_item) {
    if (t_loc.field->type == DataType::Bool && t_loc.field->count <= 1) {
        // Extract the bit from the byte we read (WLByte 1)
        bool bit_val = (static_cast<const uint8_t*>(t_item.pdata)[0] >> t_loc.field->bit_index) & 0x01;
        if (auto r = memory_.writeBit(db_num_, t_loc.abs_offset, t_loc.field->bit_index, bit_val); !r) {
            SGRN_WARN_LOG("DbIOProvider: writeBit failed for DB{}.{}: {}", db_num_, t_loc.abs_offset, toString(r.error()));
        }
    } else {
        const int sz = ::sgrn::gateway::twin::fieldSpanSize(*t_loc.field);
        if (auto r = memory_.writeDbMemory(db_num_, t_loc.abs_offset, sz, static_cast<const uint8_t*>(t_item.pdata)); !r) {
            SGRN_WARN_LOG("DbIOProvider: writeDbMemory failed for DB{}.{}: {}", db_num_, t_loc.abs_offset, toString(r.error()));
        }
    }
}

} // namespace sgrn::gateway::twin
