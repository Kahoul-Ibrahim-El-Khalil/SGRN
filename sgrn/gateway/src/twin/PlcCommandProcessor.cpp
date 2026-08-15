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

    std::vector<PlcCommand> field_commands;

    struct BinaryWrite {
        const PlcNode* node;
        DbEntry* entry;
        std::string path;
        uint64_t timestamp;
        std::vector<uint8_t> data;
    };

    std::vector<BinaryWrite> binary_writes;
    binary_writes.reserve(commands.size());

    for (auto& cmd : commands) {
        if (cmd.type == PlcCommand::WriteBinary) {
            if (cmd.value_binary.empty())
                continue;

            const PlcNode* node = memory_.state()->find(cmd.path);
            if (!node || !node->cached_slot_)
                continue;

            DbEntry* p_entry = const_cast<DbEntry*>(node->cached_slot_);
            const size_t node_start = node->offset_;

            if (node_start + cmd.value_binary.size() > p_entry->size)
                continue;

            binary_writes.push_back({node, p_entry, cmd.path, cmd.timestamp, std::move(cmd.value_binary)});
        } else {
            field_commands.push_back(std::move(cmd));
        }
    }

    // ─── Batch WriteBinary commands, grouped by DB ───────────────────────────

    if (!binary_writes.empty()) {
        std::unordered_map<uint16_t, std::vector<size_t>> indices_by_db;

        for (size_t i = 0; i < binary_writes.size(); ++i) {
            const uint16_t db = binary_writes[i].node->db_number_;
            indices_by_db[db].push_back(i);
        }

        for (auto& [db, indices] : indices_by_db) {
            std::sort(indices.begin(), indices.end(),
                [&](size_t a, size_t b) { return binary_writes[a].node->offset_ < binary_writes[b].node->offset_; });

            std::vector<DbMemorySpan> spans;
            spans.reserve(indices.size());

            for (size_t i : indices) {
                auto& bw = binary_writes[i];

                spans.push_back(DbMemorySpan{db, bw.node->offset_, bw.data.size(), bw.data.data()});
            }

            auto res = memory_.writeDbMemory(std::span(spans));

            if (res.hasError()) {
                fmt::print(stderr,
                    "[PlcCommandProcessor] Batch write to DB{} failed ({}). "
                    "Falling back to per-span writes.\n",
                    db, res.error());

                std::vector<FieldUpdateNotification> fallback_notes;
                fallback_notes.reserve(indices.size());

                for (size_t i : indices) {
                    auto& bw = binary_writes[i];

                    uint8_t* ptr = memory_.state()->arenaData() + bw.entry->offset + bw.node->offset_;

                    {
                        std::unique_lock<std::shared_mutex> lk(bw.entry->mutex_);

                        std::memcpy(ptr, bw.data.data(), bw.data.size());
                    }

                    bw.entry->last_write_ms.store(sgrn::utils::time::nowMilliseconds(), std::memory_order_release);

                    bw.entry->markDirty();

                    fallback_notes.push_back(
                        makeFieldUpdateNotification(*memory_.state(), *bw.node, *bw.entry, bw.path, bw.timestamp, true));
                }

                if (!fallback_notes.empty()) {
                    if (on_field_update_batch_) {
                        on_field_update_batch_(std::span(fallback_notes));
                    } else if (on_field_update_) {
                        for (auto& note : fallback_notes)
                            on_field_update_(std::move(note));
                    }
                }

                continue;
            }

            const uint64_t now_ms = sgrn::utils::time::nowMilliseconds();

            std::vector<FieldUpdateNotification> notifications;
            notifications.reserve(indices.size());

            for (size_t i : indices) {
                auto& bw = binary_writes[i];

                bw.entry->last_write_ms.store(now_ms, std::memory_order_release);

                notifications.push_back(makeFieldUpdateNotification(*memory_.state(), *bw.node, *bw.entry, bw.path, bw.timestamp, true));
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

    // ─── Batch WriteField commands, grouped by DB ────────────────────────────
    //
    // WriteField is the JSON/semantic write path. Previously every command
    // acquired entry->mutex_ independently. Resolve all nodes first, group
    // them by DB, then acquire one exclusive DB lock for the whole group.

    struct FieldWrite {
        const PlcNode* node;
        DbEntry* entry;
        PlcCommand* command;
    };

    std::vector<FieldWrite> field_writes;
    field_writes.reserve(field_commands.size());

    std::unordered_map<uint16_t, std::vector<size_t>> field_indices_by_db;

    for (size_t i = 0; i < field_commands.size(); ++i) {
        PlcCommand& cmd = field_commands[i];

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

        /*
         * All fields in this group belong to the same DB.
         *
         * This is the important change:
         *
         *     OLD: lock -> encode -> unlock
         *          lock -> encode -> unlock
         *          lock -> encode -> unlock
         *
         *     NEW: lock
         *              encode
         *              encode
         *              encode
         *          unlock
         */
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

        /*
         * Do bookkeeping and notifications after releasing the DB lock.
         * This preserves the existing behavior where notification generation
         * is not performed while holding entry->mutex_.
         */
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
