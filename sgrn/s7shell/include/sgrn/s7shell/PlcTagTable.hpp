#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// PlcTagTable.hpp  –  sgrn/s7 domain
//
// Sparse symbolic tag registry.
// ─────────────────────────────────────────────────────────────────────────────
#include <sgrn/TagTable.hpp>

#include <sgrn/gateway/twin/S7ChangeTracker.hpp>
#include <sgrn/gateway/wrappers/s7/ProtocolError.hpp>
#include <sgrn/scl/types.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sgrn::s7shell
{

using sgrn::gateway::wrappers::s7::S7Client;
using sgrn::gateway::wrappers::s7::S7DataItem;
using sgrn::scl::DataType;

// ─────────────────────────────────────────────────────────────────────────────
// TagAddress — PLC address parsed from the registry JSON
// ─────────────────────────────────────────────────────────────────────────────

struct TagAddress {
    int area = 0;        // S7AreaDB / S7AreaMK / S7AreaPA / S7AreaPE
    int db_number = 0;   // DB number (0 when area != S7AreaDB)
    int start_byte = 0;  // byte offset within the area
    int bit_offset = -1; // 0-7 for Bool, -1 otherwise
    int word_len = 0;    // kS7WLBit / kS7WLByte / kS7WLWord / kS7WLDWord / kS7WLReal
    int byte_count = 0;  // bytes to transfer (1,2,4 …)
};

// ─────────────────────────────────────────────────────────────────────────────
// PlcTagTable
// ─────────────────────────────────────────────────────────────────────────────

class PlcTagTable final : public ::sgrn::TagTable {
public:
    PlcTagTable() = default;

    /// Load tags from a JSON registry file.
    explicit PlcTagTable(const std::string& t_registry_path);
    ~PlcTagTable() = default;

    PlcTagTable(const PlcTagTable&) = delete;
    PlcTagTable& operator=(const PlcTagTable&) = delete;

    // ── IS7MemoryProvider ───────────────────────────────────────────────────

    using S7Error = ::sgrn::gateway::wrappers::s7::S7Error;
    using S7ProtocolCode = ::sgrn::gateway::wrappers::s7::S7ProtocolCode;

    sgrn::Result<std::string, S7Error> read(const std::string& t_path) const;
    sgrn::Result<void, S7Error> write(const std::string& t_path, const std::string& t_json_val);

    sgrn::Result<std::string, S7Error> get(S7Client& t_client, const std::string& t_path);
    sgrn::Result<void, S7Error> put(S7Client& t_client, const std::string& t_path, const std::string& t_json_val);

    sgrn::Result<void, S7Error> buildGetItems(
        const std::vector<std::string>& t_paths, std::vector<S7DataItem>& t_out_items, std::vector<std::vector<uint8_t>>& t_out_bufs);

    sgrn::Result<void, S7Error> buildPutItems(const std::vector<std::string>& t_paths,
        const std::vector<std::variant<std::string, s7codec::DecodedValue>>& t_values, std::vector<S7DataItem>& t_out_items,
        std::vector<std::vector<uint8_t>>& t_out_bufs);

    sgrn::Result<void, S7Error> commitLocalPut(
        const std::vector<std::string>& t_paths, const std::vector<std::variant<std::string, s7codec::DecodedValue>>& t_values);

    void commitGetResults(const std::vector<std::string>& t_paths, const std::vector<S7DataItem>& t_items);

    // ── Convenience ─────────────────────────────────────────────────────────

    sgrn::Result<void, ::sgrn::scl::Error> pullAll(S7Client& t_client);
    void addTag(const ::sgrn::scl::PlcTag& t_tag);
    sgrn::Result<void, ::sgrn::scl::Error> pushDirty(S7Client& t_client);
    size_t tagCount() const noexcept;

    /// Metadata for low-level / hex I/O (address, type, span).
    struct TagDescriptor {
        std::string name;
        TagAddress addr;
        std::string type_name;
        DataType type{DataType::Bool};
    };

    sgrn::Result<TagDescriptor, ::sgrn::scl::Error> describeTag(const std::string& t_name) const;
    std::vector<std::string> tagNames() const;

private:
    struct Block {
        std::string name;
        TagAddress addr;
        std::string type_name;
        DataType type;
        std::vector<uint8_t> raw_data;
        std::string json_cache;
        S7ChangeTracker tracker;
        mutable std::mutex mu;

        Block() = default;
        Block(const Block&) = delete;
    };

    std::unordered_map<std::string, std::unique_ptr<Block>> blocks_;

    sgrn::Result<Block*, ::sgrn::scl::Error> findBlock_(const std::string& t_path);
    sgrn::Result<const Block*, ::sgrn::scl::Error> findBlock_(const std::string& t_path) const;
    void fillGetItem_(const Block& t_b, S7DataItem& t_item, std::vector<uint8_t>& t_buf) const;
    sgrn::Result<std::vector<uint8_t>, ::sgrn::scl::Error> encodeValue_(const Block& t_b, const std::string& t_json_val) const;
    sgrn::Result<std::string, ::sgrn::scl::Error> decodeRaw_(const Block& t_b) const;
    sgrn::Result<void, ::sgrn::scl::Error> commitRaw_(Block& t_b, const void* tp_data, size_t t_size);
    sgrn::Result<void, ::sgrn::scl::Error> loadRegistry_(const std::string& t_path);
};
} // namespace sgrn::s7shell
