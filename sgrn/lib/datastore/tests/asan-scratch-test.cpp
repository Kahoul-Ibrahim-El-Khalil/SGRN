// sgrn/lib/datastore/tests/asan-scratch-test.cpp
//
// Acceptance scratch test for Prompt 2:
// Verifies that AddressSanitizer (ASan) catches a deliberate out-of-bounds read.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--trigger-crash") == 0) {
        std::printf("Triggering deliberate out-of-bounds read...\n");
        std::vector<int> buffer = {10, 20, 30};
        volatile const int* raw = buffer.data();
        // Reading well past the 3-element vector allocation
        volatile int leak = raw[16];
        (void)leak;
        std::printf("Value: %d (unexpected - ASan should have trapped!)\n", leak);
        return 0;
    }

    // Without --trigger-crash, test runs and passes cleanly
    std::vector<int> buffer = {1, 2, 3};
    if (buffer[0] == 1 && buffer[2] == 3) {
        std::printf("asan_scratch_test: benign run PASSED\n");
        return 0;
    }
    return 1;
}
