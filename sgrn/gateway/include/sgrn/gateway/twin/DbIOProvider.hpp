#pragma once

#include <sgrn/gateway/twin/DbIoError.hpp>
#include <sgrn/gateway/twin/DbSnapshot.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/wrappers/s7/error.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/types.hpp>
#include <cstdint>
#include <map>
#include <string>
#include <variant>
namespace sgrn::gateway::twin
{

class PlcMemory;

using ::sgrn::scl::DataType;
using ::sgrn::scl::FieldLocation;
using PlcSchemaStore = ::sgrn::scl::PlcSchemaStore;
using S7Client = ::sgrn::gateway::wrappers::s7::S7Client;
using S7DataItem = ::sgrn::gateway::wrappers::s7::S7DataItem;

class DbIOProvider final {
public:
    DbIOProvider(PlcMemory& t_memory, PlcSchemaStore& t_schema, uint16_t t_db_num, std::map<uint16_t, DbSnapshot>& t_pending_writes);
    ~DbIOProvider() = default;

    DbIOProvider(const DbIOProvider&) = delete;
    DbIOProvider& operator=(const DbIOProvider&) = delete;
    DbIOProvider(DbIOProvider&&) = default;

    // Every public entry point returns DbIoError — DbIOProvider's own error
    // domain, not PlcMemoryError (raw arena) or SclError (schema) or S7Error
    // (wire protocol), which is what it internally talks to. See DbIoError.hpp
    // for why and for the fromS7Error/fromPlcMemoryError/fromSclError bridges.
    sgrn::Result<std::string, DbIoError> read(const std::string& t_path) const;
    sgrn::Result<void, DbIoError> write(const std::string& t_path, const std::string& t_json_val);
    sgrn::Result<std::string, DbIoError> get(S7Client& t_client, const std::string& t_path);
    sgrn::Result<void, DbIoError> put(S7Client& t_client, const std::string& t_path, const std::string& t_json_val);
    sgrn::Result<void, DbIoError> put(S7Client& t_client, const std::string& t_path, const s7codec::DecodedValue& t_val);

    sgrn::Result<void, DbIoError> buildGetItems(
        const std::vector<std::string>& t_paths, std::vector<S7DataItem>& t_out_items, std::vector<std::vector<uint8_t>>& t_out_bufs);

    sgrn::Result<void, DbIoError> buildPutItems(const std::vector<std::string>& t_paths,
        const std::vector<std::variant<std::string, s7codec::DecodedValue>>& t_values, std::vector<S7DataItem>& t_out_items,
        std::vector<std::vector<uint8_t>>& t_out_bufs);

    sgrn::Result<void, DbIoError> commitLocalPut(
        const std::vector<std::string>& t_paths, const std::vector<std::variant<std::string, s7codec::DecodedValue>>& t_values);

    void commitGetResults(const std::vector<std::string>& t_paths, const std::vector<S7DataItem>& t_items);

    uint16_t getDbNumber() const noexcept {
        return db_num_;
    }

private:
    PlcMemory& memory_;
    PlcSchemaStore& schema_;
    uint16_t db_num_;
    std::map<uint16_t, DbSnapshot>& pending_writes_;

    sgrn::Result<FieldLocation, DbIoError> resolveField(const std::string& t_path) const;
    void buildOneGetItem(const FieldLocation& t_loc, S7DataItem& t_out_item, std::vector<uint8_t>& t_out_buf) const;
    sgrn::Result<std::vector<uint8_t>, DbIoError> encodeValue(const FieldLocation& t_loc, const std::string& t_json_val) const;
    sgrn::Result<std::vector<uint8_t>, DbIoError> encodeValue(const FieldLocation& t_loc, const s7codec::DecodedValue& t_val) const;
    void commitOneField(const std::string& t_path, const FieldLocation& t_loc, const S7DataItem& t_item);
};

} // namespace sgrn::gateway::twin
