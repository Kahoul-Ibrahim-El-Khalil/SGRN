// AngelScriptEngine concurrency smoke test.
//
// g_suppress_errors used to be a plain process-wide global written by
// execute() and read by the AngelScript message callback: two threads
// running execute() concurrently (two REPL sessions in one process, or the
// REPL plus a one-shot script runner) raced on it. It is now thread_local,
// which is exact because the callback fires synchronously on the thread
// that is compiling/executing.
//
// The test runs several threads, each driving its own engine through the
// exact execute() paths that toggle the flag (global-var probe, failing
// statement with print() fallbacks, valid statement), starting from a
// barrier so the suppression windows overlap. It then asserts every engine
// is still fully functional. Under ThreadSanitizer the pre-fix code reports
// a data race on the flag; the fixed code is clean.

#include <sgrn/AngelScriptEngine.hpp>

#include <scripthelper/scripthelper.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
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

using sgrn::scripting::AngelScriptEngine;

constexpr int kThreads = 4;
constexpr int kIterations = 200;

void driveEngine(AngelScriptEngine& t_engine, int t_id, std::atomic<int>& t_ready, std::atomic<bool>& t_go) {
    t_ready.fetch_add(1, std::memory_order_relaxed);
    while (!t_go.load(std::memory_order_acquire)) {
    }
    for (int i = 0; i < kIterations; ++i) {
        // Global-var probe path (suppression around CompileGlobalVar).
        t_engine.execute("int probe_" + std::to_string(t_id) + " = " + std::to_string(i));
        // Failing statement: suppression around the print() fallbacks, then
        // one unsuppressed re-run that emits the real compiler error.
        t_engine.execute("this is not valid !!! " + std::to_string(i));
        // Plain valid statement.
        t_engine.execute("print(\"\")");
    }
}

void checkEngineFunctional(AngelScriptEngine& t_engine, int t_id) {
    asIScriptContext* p_ctx = t_engine.p_script_engine_->CreateContext();
    CHECK(p_ctx != nullptr);
    if (!p_ctx)
        return;
    const std::string decl = "int angel_ok_" + std::to_string(t_id) + " = 42;";
    const int r = ExecuteString(t_engine.p_script_engine_, decl.c_str(), t_engine.p_repl_module_, p_ctx);
    CHECK(r >= 0);
    CHECK(p_ctx->GetState() != asEXECUTION_EXCEPTION);
    p_ctx->Release();
}

} // namespace

int main() {
    std::vector<std::unique_ptr<AngelScriptEngine>> engines;
    for (int t = 0; t < kThreads; ++t)
        engines.push_back(std::make_unique<AngelScriptEngine>());

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() { driveEngine(*engines[static_cast<size_t>(t)], t, ready, go); });
    }

    while (ready.load(std::memory_order_relaxed) != kThreads) {
    }
    go.store(true, std::memory_order_release);
    for (auto& t : threads)
        t.join();

    for (int t = 0; t < kThreads; ++t)
        checkEngineFunctional(*engines[static_cast<size_t>(t)], t);

    engines.clear();

    if (g_failures == 0)
        std::printf("angel_concurrency_test: ALL CHECKS PASSED (%d threads x %d iterations)\n", kThreads, kIterations);
    else
        std::printf("angel_concurrency_test: %d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
