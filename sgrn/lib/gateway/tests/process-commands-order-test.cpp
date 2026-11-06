// Deterministic DB-write-ordering test for PlcCommandProcessor.
//
// processCommands() batches queued field writes per DB. Iterating that
// batch map in unspecified (unordered_map) order makes the DB write order
// within one call vary between runs/platforms, against the project's
// determinism tenet. The batch map is ordered by DB number, so one
// processCommands() call must invoke the batch callback DB-ascending.
//
// The test queues one WriteField command per DB in scrambled order and
// asserts the batch callback observes strictly ascending DB numbers.

#include <sgrn/gateway/twin/PlcCommand.hpp>
#include <sgrn/gateway/twin/PlcCommandProcessor.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/PlcState.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;

#define CHECK(cond)                                                                                                                        \
    do {                                                                                                                                   \
        if (!(cond)) {                                                                                                                     \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                                    \
        }                                                                                                                                  \
    } while (0)

using sgrn::gateway::twin::PlcCommand;
using sgrn::gateway::twin::PlcMemory;
using sgrn::gateway::twin::PlcState;

constexpr size_t kDbSize = 16;

// Scrambled push order over six DBs (ascending by chance: 1/720).
constexpr uint16_t kPushOrder[] = {4, 1, 6, 2, 5, 3};

} // namespace

int main() {
    PlcState state;
    PlcMemory mem;
    mem.attachState(state);
    for (uint16_t db : kPushOrder) {
        CHECK(!mem.registerDb(db, kDbSize).hasError());
    }
    if (g_failures != 0)
        return 1;

    // One single-byte write per DB, queued in scrambled order. Nodes created
    // by registerDb() default to Byte scalars, so "65" always encodes.
    for (uint16_t db : kPushOrder) {
        PlcCommand cmd;
        cmd.type = PlcCommand::WriteField;
        cmd.path = "DB" + std::to_string(db);
        cmd.value_json = "65";
        cmd.timestamp = 0;
        state.pushCommand(std::move(cmd));
    }

    std::vector<uint16_t> batch_order;
    mem.processor()->setFieldUpdateBatchCallback([&](std::span<sgrn::gateway::twin::FieldUpdateNotification> t_notes) {
        for (const auto& note : t_notes)
            batch_order.push_back(note.db);
    });

    mem.processor()->processCommands();

    CHECK(batch_order.size() == 6);
    for (size_t i = 0; i < batch_order.size(); ++i) {
        if (batch_order[i] != static_cast<uint16_t>(i + 1)) {
            std::printf("FAIL batch order not ascending at index %zu (got DB%u):", i, batch_order[i]);
            for (uint16_t db : batch_order)
                std::printf(" DB%u", db);
            std::printf("\n");
            ++g_failures;
            break;
        }
    }

    if (g_failures == 0)
        std::printf("process_commands_order_test: ALL CHECKS PASSED\n");
    else
        std::printf("process_commands_order_test: %d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
