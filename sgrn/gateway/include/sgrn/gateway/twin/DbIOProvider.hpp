#pragma once

#include <sgrn/gateway/twin/DbSnapshot.hpp>
#include <sgrn/gateway/wrappers/s7/ProtocolError.hpp>
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

using S7Error = ::sgrn::gateway::wrappers::s7::S7Error;

class DbIOProvider final {
public:
    DbIOProvider(PlcMemory& t_memory, PlcSchemaStore& t_schema, uint16_t t_db_num, std::map<uint16_t, DbSnapshot>& t_pending_writes);
    ~DbIOProvider() = default;

    DbIOProvider(const DbIOProvider&) = delete;
    DbIOProvider& operator=(const DbIOProvider&) = delete;
    DbIOProvider(DbIOProvider&&) = default;

    // IS7MemoryProvider interface — return type must match base class
    sgrn::Result<std::string, S7Error> read(const std::string& t_path) const;
    sgrn::Result<void, S7Error> write(const std::string& t_path, const std::string& t_json_val);
    sgrn::Result<std::string, S7Error> get(S7Client& t_client, const std::string& t_path);
    sgrn::Result<void, S7Error> put(S7Client& t_client, const std::string& t_path, const std::string& t_json_val);

    // Non-virtual convenience overload — keeps scl::Error for internal callers
    sgrn::Result<void, ::sgrn::scl::Error> put(S7Client& t_client, const std::string& t_path, const s7codec::DecodedValue& t_val);

    sgrn::Result<void, S7Error> buildGetItems(
        const std::vector<std::string>& t_paths, std::vector<S7DataItem>& t_out_items, std::vector<std::vector<uint8_t>>& t_out_bufs);

    sgrn::Result<void, S7Error> buildPutItems(const std::vector<std::string>& t_paths,
        const std::vector<std::variant<std::string, s7codec::DecodedValue>>& t_values, std::vector<S7DataItem>& t_out_items,
        std::vector<std::vector<uint8_t>>& t_out_bufs);

    sgrn::Result<void, S7Error> commitLocalPut(
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

    sgrn::Result<FieldLocation, ::sgrn::scl::Error> resolveField(const std::string& t_path) const;
    void buildOneGetItem(const FieldLocation& t_loc, S7DataItem& t_out_item, std::vector<uint8_t>& t_out_buf) const;
    sgrn::Result<std::vector<uint8_t>, ::sgrn::scl::Error> encodeValue(const FieldLocation& t_loc, const std::string& t_json_val) const;
    sgrn::Result<std::vector<uint8_t>, ::sgrn::scl::Error> encodeValue(
        const FieldLocation& t_loc, const s7codec::DecodedValue& t_val) const;
    void commitOneField(const std::string& t_path, const FieldLocation& t_loc, const S7DataItem& t_item);
};

} // namespace sgrn::gateway::twin