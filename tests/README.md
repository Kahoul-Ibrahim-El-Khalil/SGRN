# Tests

Start from the repository root with the unified runner:

```sh
python3 tests/run_tests.py --list                  # show tests and prerequisites
python3 tests/run_tests.py gateway-rest             # run one test
python3 tests/run_tests.py --module gateway         # run one module
python3 tests/run_tests.py --kind python            # run Python tests
python3 tests/run_tests.py --kind python --module scl # just SCL Python tests
python3 tests/run_tests.py --kind ts                # run TypeScript tests
python3 tests/run_tests.py --kind cpp               # run C++ tests
python3 tests/run_tests.py all                      # run everything available
```

The runner skips entries whose prerequisites are missing and reports why. `all` skips tests marked as requiring a special setup; selecting one by name includes it. Aggregate suites are not repeated when their individual test entries are selected. Use `python3 tests/run_tests.py <name> --message` to see a test's scope and assumptions. Add a simulation name after the selection to choose the fixture scenario for Python integration tests, for example `python3 tests/run_tests.py gateway-websocket gas_processing`.

## What runs where

- **Python** tests live in `gateway/` and `scl/`. Offline tests run directly; integration tests use a temporary gateway fixture managed by the runner.
- **TypeScript** tests live in `ts/tests/`. Each file has its own gateway fixture. Run one with `python3 tests/run_tests.py ts-websocket`, or run all with `python3 tests/run_tests.py --kind ts`.
- **C++** tests live beside their modules in `sgrn/lib/*/tests/` and run through CTest. The runner discovers tests from the configured build tree. Build first if the list reports no C++ tests.

For direct commands and prerequisites, see [`WRITING_TESTS.md`](WRITING_TESTS.md). TypeScript suite details are in [`ts/TEST_README.md`](ts/TEST_README.md).
