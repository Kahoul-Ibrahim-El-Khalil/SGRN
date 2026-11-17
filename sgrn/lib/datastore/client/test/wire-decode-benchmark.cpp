// wire-decode-benchmark.cpp — micro-benchmark for the datastore client decode
// paths (not a functional test). Compares, over the same 1000 ActiveSession
// wire records decoded 10,000 times:
//
//   (a) parseValue + readFromValue  — the legacy makeRequest() path: JSON text
//                                     → dynamic sgrn::json::Value → wire type
//   (b) readJson<T>                 — direct Glaze JSON reflection
//   (c) readBeve<T>                 — direct Glaze BEVE reflection
//
// Prints ns/record for each. The binary reads no network, no database and no
// files; every input is generated in-process.

#include <sgrn/Result.hpp>
#include <sgrn/datastore/wire/admin.hpp>
#include <sgrn/utils/glaze.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{

using sgrn::datastore::admin::ActiveSession;
using DecodeResult = sgrn::Result<std::vector<ActiveSession>, std::string>;

constexpr std::size_t kRecordCount = 1000;
constexpr std::uint32_t kIterations = 10000;

std::vector<ActiveSession> makeRecords() {
    std::vector<ActiveSession> out;
    out.reserve(kRecordCount);
    for (std::size_t i = 0; i < kRecordCount; ++i) {
        ActiveSession s;
        s.token = "sess-" + std::to_string(i) + "-0123456789abcdef0123456789abcdef";
        s.user_id = static_cast<int32_t>(i % 900 + 1);
        s.automated_service_id = (i % 3 == 0) ? std::optional<int32_t>{static_cast<int32_t>(i)} : std::nullopt;
        s.ip = "10.0." + std::to_string(i / 256) + "." + std::to_string(i % 256);
        s.created_at = "2026-11-14 10:23:0" + std::to_string(i % 10);
        s.expires_at = "2026-11-15 10:23:0" + std::to_string(i % 10);
        s.actor_type = (i % 3 == 0) ? "automated_service" : "user";
        s.actor_name = (i % 3 == 0) ? "service-" + std::to_string(i) : "user" + std::to_string(i) + "@example.com";
        out.push_back(std::move(s));
    }
    return out;
}

std::size_t checksum(const std::vector<ActiveSession>& t_records) {
    std::size_t sum = t_records.size();
    for (const auto& r : t_records) {
        sum += r.token.size() + r.ip.size() + r.actor_name.size() + static_cast<std::size_t>(r.user_id.value_or(0));
    }
    return sum;
}

template <class Decode>
double benchNsPerRecord(const char* t_label, Decode&& t_decode, std::size_t t_expect_sum) {
    namespace chrono = std::chrono;
    const auto start = chrono::steady_clock::now();
    std::size_t sink = 0;
    for (std::uint32_t i = 0; i < kIterations; ++i) {
        auto decoded = t_decode();
        if (decoded.hasError() || decoded.value().size() != kRecordCount) {
            std::printf("BENCHMARK ERROR: decode failed in %s\n", t_label);
            std::exit(1);
        }
        sink += checksum(decoded.value());
    }
    const auto stop = chrono::steady_clock::now();
    if (sink != t_expect_sum * kIterations) {
        std::printf("BENCHMARK ERROR: checksum mismatch in %s\n", t_label);
        std::exit(1);
    }
    const double ns_total = static_cast<double>(chrono::duration_cast<chrono::nanoseconds>(stop - start).count());
    const double ns_per_record = ns_total / (static_cast<double>(kIterations) * static_cast<double>(kRecordCount));
    std::printf("%-28s %10.1f ns/record\n", t_label, ns_per_record);
    return ns_per_record;
}

} // namespace

int main() {
    const auto records = makeRecords();
    const std::size_t expect_sum = checksum(records);

    // Serialize the same records once per wire format.
    auto json_res = sgrn::json::write(records);
    if (json_res.hasError()) {
        std::printf("BENCHMARK ERROR: JSON serialization failed\n");
        return 1;
    }
    auto beve_res = sgrn::json::writeBeve(records);
    if (beve_res.hasError()) {
        std::printf("BENCHMARK ERROR: BEVE serialization failed\n");
        return 1;
    }
    const std::string& json_text = json_res.value();
    const std::string& beve_text = beve_res.value();

    // Sanity check: every path decodes to the same checksum before timing.
    {
        auto via_value = sgrn::json::parseValue(json_text);
        if (via_value.hasError()) {
            std::printf("BENCHMARK ERROR: parseValue failed\n");
            return 1;
        }
        auto round_trip = sgrn::json::readFromValue<std::vector<ActiveSession>>(via_value.value());
        auto direct_json = sgrn::json::read<std::vector<ActiveSession>>(json_text);
        auto direct_beve = sgrn::json::readBeve<std::vector<ActiveSession>>(beve_text);
        if (round_trip.hasError() || direct_json.hasError() || direct_beve.hasError() || checksum(round_trip.value()) != expect_sum ||
            checksum(direct_json.value()) != expect_sum || checksum(direct_beve.value()) != expect_sum) {
            std::printf("BENCHMARK ERROR: decode paths disagree\n");
            return 1;
        }
    }

    std::printf("%zu ActiveSession records x %u iterations (JSON %zu bytes, BEVE %zu bytes)\n", kRecordCount, kIterations, json_text.size(),
        beve_text.size());

    // (a) Legacy dynamic path: JSON text -> sgrn::json::Value -> wire type.
    const double legacy = benchNsPerRecord(
        "parseValue + readFromValue",
        [&]() -> DecodeResult {
            auto parsed = sgrn::json::parseValue(json_text);
            if (parsed.hasError()) {
                return DecodeResult::Error(parsed.error().what());
            }
            auto decoded = sgrn::json::readFromValue<std::vector<ActiveSession>>(parsed.value());
            if (decoded.hasError()) {
                return DecodeResult::Error(decoded.error().what());
            }
            return decoded.value();
        },
        expect_sum);

    // (b) Direct Glaze JSON reflection (what makeTypedRequest falls back to).
    const double direct_json =
        benchNsPerRecord("readJson<T>", [&] { return sgrn::json::read<std::vector<ActiveSession>>(json_text); }, expect_sum);

    // (c) Direct Glaze BEVE reflection (the makeTypedRequest fast path).
    const double direct_beve =
        benchNsPerRecord("readBeve<T>", [&] { return sgrn::json::readBeve<std::vector<ActiveSession>>(beve_text); }, expect_sum);

    std::printf("\n");
    std::printf("readJson vs legacy: %.1fx faster\n", legacy / direct_json);
    std::printf("readBeve vs legacy: %.1fx faster\n", legacy / direct_beve);
    return 0;
}
