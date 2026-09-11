// PlcMemory field/JSON query, snapshot and dirty/delta surface.
// Split from PlcMemory.cpp; read-side accessors over PlcState.
#include <fmt/core.h>
#include <sgrn/common/S7SerializationUtils.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/utils.hpp>
#include <sgrn/utils/time.hpp>
#include <algorithm>
#include <asio.hpp>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <shared_mutex>

#include <sgrn/gateway/twin/PlcCommandProcessor.hpp>
#include <sgrn/gateway/twin/SnapshotRegistry.hpp>
#include <sgrn/gateway/twin/field_update.hpp>
#include <s7codec/codec.hpp>

namespace sgrn::gateway::twin
{
using ::sgrn::scl::DataType;
using ::sgrn::scl::DbField;

class PlcState;
struct DbMemorySpan;

const PlcNode* PlcMemory::findSymbol(const std::string& t_path) const {
    return p_plc_state_ ? p_plc_state_->find(t_path) : nullptr;
}

Result<void, PlcMemoryError> PlcMemory::updateField(
    uint16_t t_db_number, const std::string& t_field_path, const std::string& t_value_json) {

    const uint64_t ts = timestamp_provider_ ? timestamp_provider_() : static_cast<uint64_t>(sgrn::utils::time::nowMilliseconds());

    return updateFieldWithTimestamp(t_db_number, t_field_path, t_value_json, ts);
}

Result<void, PlcMemoryError> PlcMemory::updateFieldWithTimestamp(
    uint16_t t_db_number, const std::string& t_field_path, const std::string& t_value_json, uint64_t t_timestamp) {

    SGRN_RETURN_IF_NULL(p_plc_state_, PlcMemoryError::PLC_STATE_NOT_INITIALIZED);

    const DbEntry* p_entry = p_plc_state_->findSegmentById(t_db_number);

    SGRN_RETURN_IF_NULL(p_entry, PlcMemoryError::DB_SEGMENT_NOT_FOUND);

    std::string path = p_entry->name;

    if (!t_field_path.empty()) {
        path += ".";

        std::string sub = t_field_path;
        std::replace(sub.begin(), sub.end(), '/', '.');

        path += sub;
    }

    PlcCommand cmd;

    cmd.type = PlcCommand::WriteField;
    cmd.path = path;
    cmd.value_json = t_value_json;
    cmd.timestamp = t_timestamp;

    p_plc_state_->pushCommand(std::move(cmd));

    signalDirty();

    return {};
}

Result<std::string, PlcMemoryError> PlcMemory::getFieldValue(uint16_t t_db_number, const std::string& t_field_path) const {

    SGRN_RETURN_IF_NULL(p_plc_state_, PlcMemoryError::PLC_STATE_NOT_INITIALIZED);

    const DbEntry* p_entry = p_plc_state_->findSegmentById(t_db_number);

    SGRN_RETURN_IF_NULL(p_entry, PlcMemoryError::DB_SEGMENT_NOT_FOUND);

    std::string path = p_entry->name;

    if (!t_field_path.empty()) {
        path += ".";

        std::string sub = t_field_path;
        std::replace(sub.begin(), sub.end(), '/', '.');

        path += sub;
    }

    std::string val = p_plc_state_->getScalarString(path);

    if (val == "null")
        return PlcMemoryError::UNMAPPED_ARENA_REGION;

    return val;
}

const PlcNode* PlcMemory::findSymbol(uint16_t t_db_number, const std::string& t_field_path) const {

    SGRN_RETURN_IF_NULL(p_plc_state_, nullptr);

    const DbEntry* p_entry = p_plc_state_->findSegmentById(t_db_number);

    SGRN_RETURN_IF(!p_entry || p_entry->name.empty(), nullptr);

    std::string path = p_entry->name;

    if (!t_field_path.empty()) {
        path += ".";

        std::string sub = t_field_path;
        std::replace(sub.begin(), sub.end(), '/', '.');

        path += sub;
    }

    return p_plc_state_->find(path);
}

Result<std::string, PlcMemoryError> PlcMemory::getDbJson(uint16_t t_db_number) const {

    SGRN_RETURN_IF_NULL(p_plc_state_, PlcMemoryError::PLC_STATE_NOT_INITIALIZED);

    const DbEntry* p_entry = p_plc_state_->findSegmentById(t_db_number);

    SGRN_RETURN_IF(!p_entry || p_entry->name.empty(), PlcMemoryError::DB_SEGMENT_NOT_FOUND);

    const std::string val = p_plc_state_->getJsonString(p_entry->name);

    if (val == "null")
        return PlcMemoryError::UNMAPPED_ARENA_REGION;

    return val;
}

Result<std::string, PlcMemoryError> PlcMemory::getSubtreeJson(uint16_t t_db_number, const std::string& t_field_path) const {

    SGRN_RETURN_IF_NULL(p_plc_state_, PlcMemoryError::PLC_STATE_NOT_INITIALIZED);

    const DbEntry* p_entry = p_plc_state_->findSegmentById(t_db_number);

    SGRN_RETURN_IF(!p_entry || p_entry->name.empty(), PlcMemoryError::DB_SEGMENT_NOT_FOUND);

    if (t_field_path.empty())
        return getDbJson(t_db_number);

    const std::string path = p_entry->name + "." + t_field_path;

    const std::string val = p_plc_state_->getJsonString(path);

    if (val == "null")
        return PlcMemoryError::UNMAPPED_ARENA_REGION;

    return val;
}

std::string PlcMemory::getDbJsonString(uint16_t t_db_number) const {

    if (!p_plc_state_)
        return "{}";

    const DbEntry* p_entry = p_plc_state_->findSegmentById(t_db_number);

    if (!p_entry || p_entry->name.empty())
        return "{}";

    const PlcNode* node = p_plc_state_->find(p_entry->name);

    if (!node)
        return "{}";

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);

    {
        std::shared_lock<std::shared_mutex> lk(p_entry->mutex_);

        node->serialize(writer, p_plc_state_->getArenaTree());

        return sb.GetString();
    }
}

std::string PlcMemory::getMemoryLayoutAsJson() const {
    return getDigitalTwinJson();
}

std::string PlcMemory::getDigitalTwinJson() const {
    return p_plc_state_ ? p_plc_state_->toJson() : "{}";
}

std::string PlcMemory::getDigitalTwinJsonString() const {
    return p_plc_state_ ? p_plc_state_->getFullSnapshot() : "{}";
}

std::vector<uint8_t> PlcMemory::getFullPlantSnapshot() const {
    if (!p_plc_state_)
        return {};

    auto& arena = p_plc_state_->getArenaTree();

    return std::vector<uint8_t>(arena.data(), arena.data() + arena.size());
}

bool PlcMemory::checkDirty() {
    if (!p_plc_state_)
        return false;

    for (auto& [name_, seg] : p_plc_state_->segments()) {
        if (seg->is_dirty_.load(std::memory_order_acquire)) {
            return true;
        }
    }

    return false;
}

std::string PlcMemory::getDeltaSnapshot(const std::vector<uint16_t>& t_filter) {
    return p_plc_state_ ? p_plc_state_->getDeltaSnapshot(t_filter) : "{}";
}

std::string PlcMemory::getDeltaSnapshotFlat(
    const ankerl::unordered_dense::map<std::string, uint32_t>& t_path_to_id, const std::vector<uint16_t>& t_filter) {
    return p_plc_state_ ? p_plc_state_->getDeltaSnapshotFlat(t_path_to_id, t_filter) : "{}";
}

std::vector<FieldUpdateNotification> PlcMemory::collectTypedDirtyLeaves(uint16_t t_db_number) {
    return p_plc_state_ ? gatherTypedDirtyLeavesByDb(*p_plc_state_, t_db_number) : std::vector<FieldUpdateNotification>{};
}

std::vector<uint16_t> PlcMemory::getDirtyDbNumbers() const {
    std::vector<uint16_t> out;

    if (!p_plc_state_)
        return out;

    for (auto& [name_, seg] : p_plc_state_->segments()) {
        if (seg->is_dirty_.load(std::memory_order_acquire)) {
            out.push_back(static_cast<uint16_t>(seg->id));
        }
    }

    return out;
}

bool PlcMemory::waitForDirty(int t_timeout_ms) {
    std::unique_lock<std::mutex> lock(dirty_cv_mutex_);

    bool triggered = dirty_cv_.wait_for(lock, std::chrono::milliseconds(t_timeout_ms), [this] { return dirty_flag_; });

    dirty_flag_ = false;

    return triggered;
}

void PlcMemory::signalDirty() {
    cmd_processor_->signalDirty();
}

} // namespace sgrn::gateway::twin
