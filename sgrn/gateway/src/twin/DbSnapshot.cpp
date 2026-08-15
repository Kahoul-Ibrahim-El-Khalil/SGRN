#include <sgrn/gateway/twin/DbSnapshot.hpp>
// DbSnapshot.cpp — management of S7 Data Block snapshots

#include <fmt/core.h>
#include <sgrn/gateway/twin/utils.hpp>
#include <sgrn/utils/endianess.hpp>
#include <sgrn/utils/strings.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <sstream>
namespace
{
namespace fs = std::filesystem;
using sgrn::Result;
using ::sgrn::scl::Err;
} // namespace

namespace sgrn::gateway::twin
{
using ::sgrn::gateway::wrappers::s7::S7Client;
using ::sgrn::scl::DataType;
using ::sgrn::scl::DbData;
using ::sgrn::scl::DbField;
using ::sgrn::scl::DbRawBuffer;
using ::sgrn::scl::DbSchema;
using ::sgrn::scl::Error;
using ::sgrn::scl::ErrorCode;
using ::sgrn::scl::SchemaCode;
using sgrn::utils::strings::trim;

// ── DbSnapshot Implementation ─────────────────────────────────────────────

DbSnapshot::DbSnapshot(DbSchema t_registry)
    : registry_(std::move(t_registry))
    , raw_buffer_(registry_.size_bytes, 0) {
}

DbSnapshot::DbSnapshot(const DbSnapshot& t_other)
    : registry_(t_other.registry_)
    , raw_buffer_(t_other.registry_.size_bytes, 0) {
    std::lock_guard lock(t_other.state_mutex_);
    // Copy the front buffer only for a new instance
    std::memcpy(raw_buffer_.front_data_rw(), t_other.raw_buffer_.front_data(), registry_.size_bytes);
    std::memcpy(raw_buffer_.back_data(), t_other.raw_buffer_.front_data(), registry_.size_bytes);
    last_sync_time_ms_ = t_other.last_sync_time_ms_.load();
    is_dirty_ = t_other.is_dirty_.load();
}

DbSnapshot& DbSnapshot::operator=(const DbSnapshot& t_other) {
    if (this != &t_other) {
        std::scoped_lock lock(state_mutex_, t_other.state_mutex_);
        registry_ = t_other.registry_;
        raw_buffer_.resize(registry_.size_bytes, 0);
        std::memcpy(raw_buffer_.front_data_rw(), t_other.raw_buffer_.front_data(), registry_.size_bytes);
        std::memcpy(raw_buffer_.back_data(), t_other.raw_buffer_.front_data(), registry_.size_bytes);
        last_sync_time_ms_ = t_other.last_sync_time_ms_.load();
        is_dirty_ = t_other.is_dirty_.load();
    }
    return *this;
}

DbSnapshot::DbSnapshot(DbSnapshot&& t_other) noexcept {
    std::lock_guard lock(t_other.state_mutex_);
    registry_ = std::move(t_other.registry_);
    raw_buffer_ = std::move(t_other.raw_buffer_);
    async_buffer_ = std::move(t_other.async_buffer_);
    last_sync_time_ms_ = t_other.last_sync_time_ms_.load();
    is_dirty_ = t_other.is_dirty_.load();
    t_other.async_in_progress_ = false;
    t_other.last_client_ = nullptr;
}

DbSnapshot& DbSnapshot::operator=(DbSnapshot&& t_other) noexcept {
    if (this != &t_other) {
        std::scoped_lock lock(state_mutex_, t_other.state_mutex_);
        registry_ = std::move(t_other.registry_);
        raw_buffer_ = std::move(t_other.raw_buffer_);
        async_buffer_ = std::move(t_other.async_buffer_);
        last_sync_time_ms_ = t_other.last_sync_time_ms_.load();
        is_dirty_ = t_other.is_dirty_.load();
        t_other.async_in_progress_ = false;
        t_other.last_client_ = nullptr;
    }
    return *this;
}

DbSnapshot::~DbSnapshot() {
    std::unique_lock lock(state_mutex_);
    if (async_in_progress_ && last_client_) {
        // Safety net: wait for Snap7 background thread to finish writing
        // to our internal buffer before we let the buffer be freed.
        auto* p_client = last_client_;
        lock.unlock();
        (void)p_client->waitAsyncCompletion(1000);
    }
}

sgrn::Result<void, ::sgrn::scl::Error> DbSnapshot::read(S7Client& t_client) {
    if (registry_.size_bytes == 0) {
        return {};
    }
    std::lock_guard lock(state_mutex_);
    if (async_in_progress_) {
        return Error{SchemaCode::Generic, "Cannot perform synchronous read while an async read is in progress"};
    }

    auto proto_res = t_client.readDB(registry_.db_number, 0, registry_.size_bytes, raw_buffer_.back_data());
    if (proto_res.hasError())
        return Error{SchemaCode::Generic, proto_res.error().string()};

    raw_buffer_.swap();
    last_sync_time_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    is_dirty_ = false;
    return {};
}

sgrn::Result<void, ::sgrn::scl::Error> DbSnapshot::beginAsyncRead(S7Client& t_client) {
    if (registry_.size_bytes == 0) {
        return {};
    }
    std::lock_guard lock(state_mutex_);
    if (async_in_progress_) {
        return Error{SchemaCode::Generic, "Async read already in progress"};
    }

    last_client_ = &t_client;
    async_in_progress_ = true;
    if (async_buffer_.size() != static_cast<size_t>(registry_.size_bytes)) {
        async_buffer_.resize(registry_.size_bytes);
    }
    auto proto_res = t_client.asyncReadDB(registry_.db_number, 0, registry_.size_bytes, async_buffer_.data());
    if (proto_res.hasError()) {
        async_in_progress_ = false;
        last_client_ = nullptr;
        return Error{SchemaCode::Generic, proto_res.error().string()};
    }
    return {};
}

void DbSnapshot::commitAsyncRead() {
    std::lock_guard lock(state_mutex_);
    if (!async_in_progress_)
        return;

    if (async_buffer_.size() == static_cast<size_t>(registry_.size_bytes)) {
        std::memcpy(raw_buffer_.back_data(), async_buffer_.data(), registry_.size_bytes);
    }
    raw_buffer_.swap();
    last_sync_time_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    is_dirty_ = false;
    async_in_progress_ = false;
    last_client_ = nullptr;
}

sgrn::Result<void, ::sgrn::scl::Error> DbSnapshot::write(S7Client& t_client) {
    if (registry_.size_bytes == 0)
        return {};
    std::lock_guard lock(state_mutex_);
    if (async_in_progress_) {
        return Error{SchemaCode::Generic, "Cannot perform synchronous write while an async read is in progress"};
    }
    auto proto_res = t_client.writeDB(registry_.db_number, 0, registry_.size_bytes, raw_buffer_.front_data());
    if (proto_res.hasError())
        return Error{SchemaCode::Generic, proto_res.error().string()};

    is_dirty_ = false;
    return {};
}

// ── RapidJSON Serialization Helpers ──────────────────────────────────────

namespace
{
using namespace rapidjson;
} // namespace

sgrn::Result<DbData, ::sgrn::scl::Error> DbSnapshot::decode() const {
    // Wait-free check on buffer size.
    // registry_ is immutable after construction.
    if (raw_buffer_.size() < static_cast<size_t>(registry_.size_bytes)) {
        return Error{SchemaCode::SerializationError, "Buffer size mismatch"};
    }

    if (registry_.max_depth > 16) {
        return Error{SchemaCode::SerializationError, "Max depth exceeded"};
    }

    StringBuffer sb;
    Writer<StringBuffer> writer(sb);

    writer.StartObject();
    const uint8_t* p_data = raw_buffer_.front_data();
    for (const DbField& field : registry_.fields) {
        writer.Key(field.name.c_str());
        serializeFieldToWriter(writer, field, p_data + field.offset, registry_.size_bytes - field.offset, 0);
    }
    writer.EndObject();

    return std::string(sb.GetString());
}

sgrn::Result<void, ::sgrn::scl::Error> DbSnapshot::updateField(const std::string& t_field_path, const std::string& t_value) {
    std::lock_guard lock(state_mutex_);
    std::optional<LocatedField> located = ::sgrn::gateway::twin::findFieldByPath(registry_.fields, t_field_path);
    if (!located.has_value()) {
        return Err::Generic("field '{}' not found in DB{}", t_field_path, registry_.db_number);
    }
    sgrn::Result<void, ::sgrn::scl::Error> status = ::sgrn::gateway::twin::encodeValue(*located.value().field, t_value,
        raw_buffer_.front_data_rw() + located.value().abs_offset, registry_.size_bytes - located.value().abs_offset);
    if (status.hasError())
        return status;

    // For "Kill" optimization, we update the back buffer too so they stay in sync
    std::memcpy(raw_buffer_.back_data() + located.value().abs_offset, raw_buffer_.front_data() + located.value().abs_offset,
        ::sgrn::gateway::twin::fieldSpanSize(*located.value().field));

    is_dirty_ = true;
    return {};
}

namespace
{

sgrn::Result<void, ::sgrn::scl::Error> applyRapidJsonPatch(
    const std::vector<DbField>& t_fields, const rapidjson::Value& t_patch, uint8_t* tp_ptr, size_t t_buffer_size) {
    if (!t_patch.IsObject())
        return Error{SchemaCode::Generic, "Patch must be an object"};

    for (const auto& field : t_fields) {
        if (!t_patch.HasMember(field.name.c_str()))
            continue;

        const auto& val = t_patch[field.name.c_str()];
        uint8_t* field_ptr = tp_ptr + field.offset;
        size_t field_buffer_remaining = t_buffer_size - field.offset;

        // Structs recurse directly; all other types bridge through rapidjson
        if (field.type == DataType::Struct && field.count <= 1 && val.IsObject()) {
            auto status = applyRapidJsonPatch(field.children, val, field_ptr, field_buffer_remaining);
            if (status.hasError())
                return status;
        } else {
            auto status = ::sgrn::gateway::twin::encodeFieldRapidJson(field, val, field_ptr, field_buffer_remaining);
            if (status.hasError())
                return status;
        }
    }
    return {};
}
} // namespace

sgrn::Result<void, ::sgrn::scl::Error> DbSnapshot::updateFromJson(const std::string& t_json_patch) {
    rapidjson::Document doc;
    if (doc.Parse(t_json_patch.c_str()).HasParseError()) {
        return Error{SchemaCode::ParseError, "Invalid JSON patch string"};
    }

    std::lock_guard lock(state_mutex_);
    auto res = applyRapidJsonPatch(registry_.fields, doc, raw_buffer_.front_data_rw(), registry_.size_bytes);
    if (!res.hasError()) {
        // Sync back buffer
        std::memcpy(raw_buffer_.back_data(), raw_buffer_.front_data(), registry_.size_bytes);
        is_dirty_ = true;
    }
    return res;
}

sgrn::Result<void, ::sgrn::scl::Error> DbSnapshot::readField(S7Client& t_client, const std::string& t_field_path) {
    std::lock_guard lock(state_mutex_);
    if (async_in_progress_) {
        return Error{SchemaCode::Generic, "Cannot perform field read while an async read is in progress"};
    }
    auto located = ::sgrn::gateway::twin::findFieldByPath(registry_.fields, t_field_path);
    if (!located.has_value())
        return Err::Generic("field '{}' not found in DB{}", t_field_path, registry_.db_number);

    const int size = ::sgrn::gateway::twin::fieldSpanSize(*located.value().field);
    auto proto_res = t_client.readDB(registry_.db_number, located.value().abs_offset, size);
    if (proto_res.hasError())
        return Error{SchemaCode::Generic, proto_res.error().string()};
    const DbRawBuffer& res_buf = proto_res.value();

    std::memcpy(raw_buffer_.front_data_rw() + located.value().abs_offset, res_buf.data(), size);
    std::memcpy(raw_buffer_.back_data() + located.value().abs_offset, res_buf.data(), size);
    last_sync_time_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return {};
}

sgrn::Result<void, ::sgrn::scl::Error> DbSnapshot::writeField(
    S7Client& t_client, const std::string& t_field_path, const std::string& t_value) {
    std::lock_guard lock(state_mutex_);
    if (async_in_progress_) {
        return Error{SchemaCode::Generic, "Cannot perform field write while an async read is in progress"};
    }
    std::optional<LocatedField> located = ::sgrn::gateway::twin::findFieldByPath(registry_.fields, t_field_path);
    if (!located.has_value())
        return Err::Generic("field '{}' not found in DB{}", t_field_path, registry_.db_number);
    const int abs_offset = located.value().abs_offset;
    const DbField& field = *located.value().field;
    const int size = ::sgrn::gateway::twin::fieldSpanSize(field);

    if (field.type == DataType::Bool) {
        auto cur_res = t_client.readDB(registry_.db_number, abs_offset, size);
        if (cur_res.hasError())
            return Error{SchemaCode::Generic, cur_res.error().string()};
        std::memcpy(raw_buffer_.front_data_rw() + abs_offset, cur_res.value().data(), size);
        std::memcpy(raw_buffer_.back_data() + abs_offset, cur_res.value().data(), size);
    }

    auto encode_status =
        ::sgrn::gateway::twin::encodeFieldAt(field, t_value, raw_buffer_.front_data_rw() + abs_offset, registry_.size_bytes - abs_offset);
    if (encode_status.hasError())
        return encode_status;

    auto write_res = t_client.writeDB(registry_.db_number, abs_offset, size, raw_buffer_.front_data() + abs_offset);
    if (write_res.hasError())
        return Error{SchemaCode::Generic, write_res.error().string()};

    // Sync back buffer
    std::memcpy(raw_buffer_.back_data() + abs_offset, raw_buffer_.front_data() + abs_offset, size);
    return {};
}

sgrn::Result<std::string, ::sgrn::scl::Error> DbSnapshot::getFieldValue(const std::string& t_field_path) const {
    // Wait-free read from front data
    std::optional<LocatedField> located = ::sgrn::gateway::twin::findFieldByPath(registry_.fields, t_field_path);
    if (!located.has_value())
        return Error{SchemaCode::Generic, "Field not found"};

    return ::sgrn::gateway::twin::decodeFieldAt(
        *located.value().field, raw_buffer_.front_data() + located.value().abs_offset, registry_.size_bytes - located.value().abs_offset);
}

void DbSnapshot::updateRawRegion(size_t t_offset, const uint8_t* tp_src, size_t t_bytes) {
    if (!tp_src || t_bytes == 0)
        return;

    std::lock_guard lock(state_mutex_);

    // If an async PLC read is already in flight, its commitAsyncRead() will shortly overwrite
    // the back buffer with authoritative PLC data — patching now would either be overwritten
    // (back buffer) or race with the swap (front buffer).  The arena already holds the correct
    // value, so skip the patch and let the next poll cycle pick it up naturally.
    if (async_in_progress_) {
        fmt::print("[DbSnapshot] DB{}: skipping updateRawRegion — async read in flight; arena is authoritative\n", registry_.db_number);
        return;
    }

    // Guard against out-of-bounds writes.
    if (t_offset + t_bytes > static_cast<size_t>(registry_.size_bytes)) {
        fmt::print("[DbSnapshot] DB{}: updateRawRegion out of bounds (offset={} bytes={} db_size={})\n", registry_.db_number, t_offset,
            t_bytes, registry_.size_bytes);
        return;
    }

    // Mirror the two-buffer sync pattern used in updateField() / writeField():
    // patch both front (readers) and back (next swap target) so they stay in sync.
    std::memcpy(raw_buffer_.front_data_rw() + t_offset, tp_src, t_bytes);
    std::memcpy(raw_buffer_.back_data() + t_offset, tp_src, t_bytes);

    is_dirty_ = true;
    last_sync_time_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace sgrn::gateway::twin
