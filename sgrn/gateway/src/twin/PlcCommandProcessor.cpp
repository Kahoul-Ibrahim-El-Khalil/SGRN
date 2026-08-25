#include <sgrn/gateway/twin/PlcCommand.hpp>
#include <sgrn/gateway/twin/PlcCommandProcessor.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/encoding.hpp>
#include <sgrn/gateway/twin/field_update.hpp>
#include <sgrn/gateway/twin/path.hpp>
#include <sgrn/utils/time.hpp>
#include <asio.hpp>
#include <cstring>
#include <shared_mutex>

namespace sgrn::gateway::twin
{

bool PlcCommandProcessor::hasPendingCommands() const {
    if (!memory_.state())
        return false;
    return memory_.state()->hasPendingCommands();
}

void PlcCommandProcessor::processCommands() {
    if (!memory_.state())
        return;

    auto commands = memory_.state()->drainCommands();
    if (commands.empty())
        return;

    // WriteBinary no longer exists as a queued command type — OPC UA now
    // commits synchronously via PlcMemory::writeDbMemory() in writeValue.cpp.
    // Only WriteField (the generic path/JSON semantic-write API) still
    // flows through this queue.
    struct FieldWrite {
        const PlcNode* node;
        DbEntry* entry;
        PlcCommand* command;
    };

    std::vector<FieldWrite> field_writes;
    field_writes.reserve(commands.size());

    std::unordered_map<uint16_t, std::vector<size_t>> field_indices_by_db;

    for (size_t i = 0; i < commands.size(); ++i) {
        PlcCommand& cmd = commands[i];

        const PlcNode* node = memory_.state()->find(cmd.path);

        if (!node || !node->cached_slot_)
            continue;

        DbEntry* entry = const_cast<DbEntry*>(node->cached_slot_);

        if (node->offset_ >= entry->size)
            continue;

        const size_t index = field_writes.size();

        field_writes.push_back(FieldWrite{node, entry, &cmd});

        field_indices_by_db[node->db_number_].push_back(index);
    }

    for (auto& [db, indices] : field_indices_by_db) {
        if (indices.empty())
            continue;

        std::vector<uint8_t> succeeded(indices.size(), 0);

        DbEntry* entry = field_writes[indices.front()].entry;

        {
            std::unique_lock<std::shared_mutex> lk(entry->mutex_);

            for (size_t local_i = 0; local_i < indices.size(); ++local_i) {

                FieldWrite& fw = field_writes[indices[local_i]];

                PlcCommand& cmd = *fw.command;

                const PlcNode* node = fw.node;

                DbField field;

                if (node->children_.empty()) {
                    field.name = node->name_;
                    field.type = node->type_;
                    field.count = static_cast<int>(node->count_);
                    field.bit_index = static_cast<int>(node->bit_index_);
                    field.endianness = node->endian_;
                    field.offset = 0;
                } else {
                    field = plcNodeToDbField(*node);
                }

                const size_t node_start = node->offset_;

                if (node_start >= fw.entry->size)
                    continue;

                uint8_t* ptr = memory_.state()->arenaData() + fw.entry->offset + node_start;

                const size_t buf_remaining = fw.entry->size - node_start;

                auto res = ::sgrn::gateway::twin::encodeFieldAt(field, cmd.value_json, ptr, buf_remaining, 0, node->endian_);

                if (!res.hasError())
                    succeeded[local_i] = 1;
            }
        }

        std::vector<FieldUpdateNotification> notifications;
        notifications.reserve(indices.size());

        const uint64_t now_ms = sgrn::utils::time::nowMilliseconds();

        for (size_t local_i = 0; local_i < indices.size(); ++local_i) {

            if (!succeeded[local_i])
                continue;

            FieldWrite& fw = field_writes[indices[local_i]];

            PlcCommand& cmd = *fw.command;

            fw.entry->last_write_ms.store(now_ms, std::memory_order_release);

            fw.entry->markDirty();

            // NOTE: still needs the same version-bump fix as bumpFieldVersions
            // below — this path writes into the arena directly and never
            // calls incrementNodeVersion, so it's worth routing through
            // memory_.write() here too in a follow-up pass. Flagging it,
            // not fixing it in this diff since you asked specifically for
            // the OPC UA direct-write refactor.

            auto note = makeFieldUpdateNotification(*memory_.state(), *fw.node, *fw.entry, cmd.path, cmd.timestamp);

            note.json_value = cmd.value_json;

            notifications.push_back(std::move(note));
        }

        if (!notifications.empty()) {
            if (on_field_update_batch_) {
                on_field_update_batch_(std::span(notifications));
            } else if (on_field_update_) {
                for (auto& note : notifications)
                    on_field_update_(std::move(note));
            }
        }
    }
}
void PlcCommandProcessor::processDirty() {
    processCommands();

    if (!memory_.checkDirty()) {
        return;
    }

    std::vector<std::string> dirty_paths = memory_.getDirtyPaths();

    if (heavy_pool_ && dirty_callback_) {
        asio::post(*heavy_pool_, [cb = dirty_callback_, paths = std::move(dirty_paths)]() { cb(std::move(paths)); });
    }
}

void PlcCommandProcessor::signalDirty() {
    if (light_ctx_) {
        asio::post(*light_ctx_, [this]() { this->processDirty(); });
    }
}

} // namespace sgrn::gateway::twin
