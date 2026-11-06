// Twin port glue test.
//
// Proves TwinMemoryPort forwards the real twin's failures onto the exact
// port vocabulary the adapters switch on, for every PlcMemoryError value:
// twin behavior on one side, FakeMemoryPort-compatible classes on the other.
// Unlike adapter_ports_test, this TU deliberately links the twin — it tests
// the glue, not the decoupling.

#include <sgrn/common/ErrorClass.hpp>
#include <sgrn/gateway/adapters/ports/TwinPorts.hpp>
#include <sgrn/gateway/common/ErrorClass.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>

#include <cstdio>
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

using sgrn::common::ErrorClass;
using sgrn::gateway::twin::PlcMemory;
using sgrn::gateway::twin::PlcMemoryError;

void checkClassifyTotal() {
    // classify() is total over the enum: the seven documented cases plus the
    // two legacy extras, which fall through to Internal (Task 1 semantics).
    CHECK(sgrn::gateway::common::classify(PlcMemoryError::PLC_STATE_NOT_INITIALIZED) == ErrorClass::NotInitialized);
    CHECK(sgrn::gateway::common::classify(PlcMemoryError::DB_SEGMENT_NOT_FOUND) == ErrorClass::NotFound);
    CHECK(sgrn::gateway::common::classify(PlcMemoryError::UNMAPPED_ARENA_REGION) == ErrorClass::NotFound);
    CHECK(sgrn::gateway::common::classify(PlcMemoryError::RANGE_EXCEEDS_ALLOWED_SPACE) == ErrorClass::OutOfRange);
    CHECK(sgrn::gateway::common::classify(PlcMemoryError::RANGE_CROSSES_SEGMENT_BOUNDARY) == ErrorClass::OutOfRange);
    CHECK(sgrn::gateway::common::classify(PlcMemoryError::NULL_BUFFER) == ErrorClass::Internal);
    CHECK(sgrn::gateway::common::classify(PlcMemoryError::INVALID_BIT_INDEX) == ErrorClass::Internal);
    CHECK(sgrn::gateway::common::classify(PlcMemoryError::UKNOWN) == ErrorClass::Internal);
    CHECK(sgrn::gateway::common::classify(PlcMemoryError::EXTERNAL) == ErrorClass::Internal);
}

void checkUnattachedTwin() {
    // A never-attached PlcMemory fails everything with NOT_INITIALIZED, so
    // the port must surface NotInitialized on every op.
    PlcMemory twin;
    sgrn::gateway::adapters::ports::TwinMemoryPort port(twin);

    CHECK(!port.isReady());
    CHECK(port.topLevelDbNumbers().empty());
    CHECK(!port.dbSize(1).has_value());

    uint8_t buf[4] = {};
    const uint8_t w[4] = {1, 2, 3, 4};
    CHECK(port.readDbMemory(1, 0, 4, buf).error() == ErrorClass::NotInitialized);
    CHECK(port.writeDbMemory(1, 0, 4, w).error() == ErrorClass::NotInitialized);
    CHECK(port.writeBit(1, 0, 0, true).error() == ErrorClass::NotInitialized);
    CHECK(port.updateField(1, "speed", "1.0").error() == ErrorClass::NotInitialized);

    std::vector<sgrn::common::DbMemorySpan> spans = {{.db = 1, .offset = 0, .size = 4, .p_buffer = buf}};
    CHECK(port.readDbMemory(std::span(spans)).error() == ErrorClass::NotInitialized);
}

} // namespace

int main() {
    checkClassifyTotal();
    checkUnattachedTwin();
    if (g_failures == 0)
        std::printf("twin_ports_test: ALL CHECKS PASSED\n");
    else
        std::printf("twin_ports_test: %d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
