// CRUD view bind regression tests.
//
// executeViewInsert/executeViewUpdate used to bind every JSON body value
// with asString() regardless of the column's FieldType. jsoncpp's asString()
// converts scalars (so plain ints/bools happened to work), but mismatched
// types reached Postgres as garbage (500 instead of 400), and JSON
// objects/arrays THREW Json::LogicError out of the coroutine — including
// for jsonb columns, where objects are the valid values.
//
// fieldValueToBind() now validates each value against its FieldType and
// renders the canonical text bind; these tests cover, per FieldType:
//   - a valid value binds to its canonical form (helper level),
//   - a mismatched JSON type is rejected (helper level + HTTP 400 on both
//     the insert and the update path, without needing a live database:
//     validation runs before any DB access).

#include <sgrn/datastore/core/db.hpp>
#include <sgrn/datastore/query/CrudViewEngine.hpp>
#include <sgrn/datastore/query/CrudViewSpec.hpp>

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <json/json.h>

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

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

using sgrn::datastore::query::CrudViewSpec;
using sgrn::datastore::query::executeViewGet;
using sgrn::datastore::query::executeViewInsert;
using sgrn::datastore::query::executeViewList;
using sgrn::datastore::query::executeViewUpdate;
using sgrn::datastore::query::Field;
using sgrn::datastore::query::FieldType;
using sgrn::datastore::query::fieldValueToBind;
using sgrn::datastore::query::Op;
using sgrn::datastore::query::PrimaryKey;

Json::Value parse(std::string_view t_json) {
    Json::Value v;
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(t_json.data(), t_json.data() + t_json.size(), &v, &errors)) {
        ++g_failures;
        std::printf("FAIL test JSON does not parse: %.*s (%s)\n", static_cast<int>(t_json.size()), t_json.data(), errors.c_str());
    }
    return v;
}

void checkBindOk(FieldType t_type, const Json::Value& t_value, const std::string& t_expected) {
    auto res = fieldValueToBind("col", t_type, t_value);
    if (res.hasError()) {
        ++g_failures;
        std::printf("FAIL bind unexpectedly rejected (%s)\n", res.error().c_str());
        return;
    }
    CHECK(res.value() == t_expected);
}

void checkBindRejected(FieldType t_type, const Json::Value& t_value) {
    auto res = fieldValueToBind("col", t_type, t_value);
    if (!res.hasError()) {
        ++g_failures;
        std::printf("FAIL bind unexpectedly accepted: '%s'\n", res.value().c_str());
    }
}

// Spec with one insertable/updatable column per FieldType. Static storage:
// CrudViewSpec only holds views/spans into these.
const Field kFields[] = {
    {.name = "c_int", .type = FieldType::Int, .filter_ops = 0, .insertable = true, .updatable = true},
    {.name = "c_bigint", .type = FieldType::BigInt, .filter_ops = 0, .insertable = true, .updatable = true},
    {.name = "c_text", .type = FieldType::Text, .filter_ops = 0, .insertable = true, .updatable = true},
    {.name = "c_bool", .type = FieldType::Bool, .filter_ops = 0, .insertable = true, .updatable = true},
    {.name = "c_ts", .type = FieldType::Timestamp, .filter_ops = 0, .insertable = true, .updatable = true},
    {.name = "c_jsonb", .type = FieldType::Jsonb, .filter_ops = 0, .insertable = true, .updatable = true},
};

CrudViewSpec testSpec() {
    CrudViewSpec spec;
    spec.read_relation = "v_test";
    spec.write_table = "t_test";
    spec.tenant_column = "tenant_id";
    spec.pk = PrimaryKey{.name = "id", .type = FieldType::Int};
    spec.fields = kFields;
    spec.default_order = "id";
    spec.max_limit = 10;
    return spec;
}

int postStatus(const Json::Value& t_body) {
    auto req = drogon::HttpRequest::newHttpJsonRequest(t_body);
    auto resp = drogon::sync_wait(executeViewInsert(testSpec(), req, "tenant-1"));
    return static_cast<int>(resp->getStatusCode());
}

int patchStatus(const Json::Value& t_body) {
    auto req = drogon::HttpRequest::newHttpJsonRequest(t_body);
    auto resp = drogon::sync_wait(executeViewUpdate(testSpec(), req, "tenant-1", "1"));
    return static_cast<int>(resp->getStatusCode());
}

void checkHttpRejected(const std::string& t_json) {
    const Json::Value body = parse(t_json);
    CHECK(postStatus(body) == 400);
    CHECK(patchStatus(body) == 400);
}

// --- wire-format section (live DB, auto-skip) ------------------------------
// Output serialization (fieldToJson/rowToJson for get/insert/update, the
// raw-string builder for list) operates on drogon::orm::Field, which can
// only come from a real query result — so these cases need a live Postgres.
// Connection comes from PGHOST/PGPORT/PGDATABASE/PGUSER/PGPASSWORD (dev
// defaults 127.0.0.1/5432/sgrn/sgrn_datastore/dracaeris, same as the
// generators). If no server answers, the whole section prints SKIP and the
// binary still exits 0: ctest never requires a database.

std::string dbEnv(const char* t_name, const char* t_dflt) {
    const char* v = std::getenv(t_name);
    return (v != nullptr && *v != '\0') ? v : t_dflt;
}

int dbPort() {
    try {
        return std::stoi(dbEnv("PGPORT", "5432"));
    } catch (const std::exception&) {
        return 5432;
    }
}

bool tcpReachable(const std::string& t_host, int t_port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    if (::getaddrinfo(t_host.c_str(), std::to_string(t_port).c_str(), &hints, &list) != 0) {
        return false;
    }
    bool ok = false;
    for (addrinfo* ai = list; ai != nullptr && !ok; ai = ai->ai_next) {
        const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            ok = true;
        } else if (errno == EINPROGRESS) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            const timeval tv{0, 300000};
            if (::select(fd + 1, nullptr, &wfds, nullptr, const_cast<timeval*>(&tv)) > 0) {
                int err = 0;
                socklen_t len = sizeof(err);
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                ok = (err == 0);
            }
        }
        ::close(fd);
    }
    ::freeaddrinfo(list);
    return ok;
}

// One column per FieldType, plus a nasty-text column (escaping torture) and
// a TEXT column declared Jsonb (lets corrupt text reach the jsonb output
// paths — a real jsonb column would reject it at write time).
const Field kWireFields[] = {
    {.name = "c_int", .type = FieldType::Int, .filter_ops = 0, .insertable = true, .updatable = true},
    {.name = "c_bigint", .type = FieldType::BigInt, .filter_ops = 0, .insertable = true, .updatable = true},
    {.name = "c_text", .type = FieldType::Text, .filter_ops = 0, .insertable = true, .updatable = true},
    {.name = "c_bool", .type = FieldType::Bool, .filter_ops = 0, .insertable = true, .updatable = true},
    {.name = "c_ts", .type = FieldType::Timestamp, .filter_ops = 0, .insertable = true, .updatable = true},
    {.name = "c_jsonb", .type = FieldType::Jsonb, .filter_ops = 0, .insertable = true, .updatable = true},
    {.name = "c_nasty", .type = FieldType::Text, .filter_ops = 0, .insertable = true, .updatable = true},
    {.name = "c_fakejsonb", .type = FieldType::Jsonb, .filter_ops = 0, .insertable = true, .updatable = true},
};

CrudViewSpec wireSpec() {
    CrudViewSpec spec;
    spec.read_relation = "crud_wire_test";
    spec.write_table = "crud_wire_test";
    spec.tenant_column = "tenant";
    spec.pk = PrimaryKey{.name = "id", .type = FieldType::Int};
    spec.fields = kWireFields;
    spec.default_order = "id";
    spec.max_limit = 10;
    return spec;
}

std::string nastyText() {
    // Quote, backslash, newline, tab, non-ASCII UTF-8 (é = C3 A9, ✓ = E2 9C
    // 93), plus control bytes that must become \u00xx and DEL (0x7f, passes
    // through — only bytes below 0x20 are escaped).
    std::string s = "q\"uote\\back\nline\ttab caf\xC3\xA9 \xE2\x9C\x93";
    s.push_back('\b');
    s.push_back('\f');
    s.push_back('\x1f');
    s.push_back('\x7f');
    return s;
}

const char* kWireNullCols[] = {"c_int", "c_bigint", "c_text", "c_bool", "c_ts", "c_jsonb", "c_nasty", "c_fakejsonb"};

void checkFullRow(const Json::Value& r) {
    CHECK(r["id"].isInt());
    CHECK(r["tenant"].isString() && r["tenant"].asString() == "t1");
    CHECK(r["c_int"].isInt() && r["c_int"].asInt() == 42);
    CHECK(r["c_bigint"].isString() && r["c_bigint"].asString() == "9007199254740993"); // >2^53: must stay a string
    CHECK(r["c_text"].isString() && r["c_text"].asString() == "hello");
    CHECK(r["c_bool"].isBool() && r["c_bool"].asBool());
    CHECK(r["c_ts"].isString() && !r["c_ts"].asString().empty()); // exact text is TZ-dependent; type is the point
    CHECK(r["c_jsonb"] == parse("{\"a\":1,\"b\":[1,2],\"c\":{\"d\":null},\"e\":{}}"));
    CHECK(r["c_nasty"].isString() && r["c_nasty"].asString() == nastyText());
    CHECK(r["c_fakejsonb"].isString() && r["c_fakejsonb"].asString() == "{oops"); // corrupt text degrades, never corrupts
}

void checkNullRow(const Json::Value& r) {
    CHECK(r["id"].isInt());
    CHECK(r["tenant"].isString() && r["tenant"].asString() == "t1");
    for (const char* col : kWireNullCols) {
        if (!r[col].isNull()) {
            ++g_failures;
            std::printf("FAIL %s:%d: expected null for %s\n", __FILE__, __LINE__, col);
        }
    }
}

void execSetup(drogon::orm::DbClientPtr t_client, const std::string& t_sql, const std::vector<std::string>& t_binds = {}) {
    drogon::sync_wait(sgrn::datastore::core::execSqlCoroVec(t_client, t_sql, t_binds));
}

// Row id, tolerant of wire shape: the full row is identified by its text
// marker (never by a type assertion), so a rendering regression fails its
// own CHECK instead of misrouting rows and cascading noise.
bool isFullRow(const Json::Value& r) {
    return r["c_text"].isString() && r["c_text"].asString() == "hello";
}

int rowId(const Json::Value& r) {
    if (r["id"].isInt()) {
        return r["id"].asInt();
    }
    return std::stoi(r["id"].asString());
}

void runWireFormatTests() {
    auto client = drogon::app().getDbClient();
    if (!client) {
        std::printf("crud_binds_test: SKIP wire-format section (db client unavailable)\n");
        return;
    }
    try {
        execSetup(client, "DROP TABLE IF EXISTS crud_wire_test");
        execSetup(client, "CREATE TABLE crud_wire_test ("
                          "id SERIAL PRIMARY KEY, tenant TEXT NOT NULL, "
                          "c_int INT, c_bigint BIGINT, c_text TEXT, c_bool BOOLEAN, "
                          "c_ts TIMESTAMPTZ, c_jsonb JSONB, c_nasty TEXT, c_fakejsonb TEXT)");
        execSetup(client,
            "INSERT INTO crud_wire_test (tenant, c_int, c_bigint, c_text, c_bool, c_ts, c_jsonb, c_nasty, c_fakejsonb)"
            " VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9)",
            {"t1", "42", "9007199254740993", "hello", "true", "2024-01-15 10:30:00+00", "{\"a\":1,\"b\":[1,2],\"c\":{\"d\":null},\"e\":{}}",
                nastyText(), "{oops"});
        execSetup(client, "INSERT INTO crud_wire_test (tenant) VALUES ($1)", {"t1"});
    } catch (const std::exception& e) {
        // Reachable port but unusable database (creds, perms, wrong server):
        // nothing learned, skip rather than fail.
        std::printf("crud_binds_test: SKIP wire-format section (setup failed: %s)\n", e.what());
        return;
    }

    // --- LIST (raw-string path) -------------------------------------------
    const std::string list_body{drogon::sync_wait(executeViewList(wireSpec(), drogon::HttpRequest::newHttpRequest(), "t1"))->getBody()};
    const Json::Value list = parse(list_body); // must parse: proves the raw builder emits valid JSON throughout
    if (!list.isArray() || list.size() != 2) {
        ++g_failures;
        std::printf("FAIL %s:%d: expected 2 list rows\n", __FILE__, __LINE__);
    } else {
        // Sharp wire-format checks on the raw text itself (our builder emits
        // no spaces, so these substrings are exact): native number/bool
        // unquoted, bigint quoted.
        CHECK(list_body.find("\"c_int\":42") != std::string::npos);
        CHECK(list_body.find("\"c_bigint\":\"9007199254740993\"") != std::string::npos);
        CHECK(list_body.find("\"c_bool\":true") != std::string::npos);
        const Json::Value& first = list[0];
        const Json::Value& second = list[1];
        const bool first_is_full = isFullRow(first);
        checkFullRow(first_is_full ? first : second);
        checkNullRow(first_is_full ? second : first);

        // --- GET (Json::Value path) on both rows ---------------------------
        for (const Json::Value& row : {first, second}) {
            const bool full = isFullRow(row);
            auto get_resp =
                drogon::sync_wait(executeViewGet(wireSpec(), drogon::HttpRequest::newHttpRequest(), "t1", std::to_string(rowId(row))));
            CHECK(static_cast<int>(get_resp->getStatusCode()) == 200);
            const Json::Value got = parse(get_resp->getBody());
            if (full) {
                checkFullRow(got);
            } else {
                checkNullRow(got);
            }
        }
        // Cross-tenant get stays 404 (negative control on untouched logic).
        auto x_resp =
            drogon::sync_wait(executeViewGet(wireSpec(), drogon::HttpRequest::newHttpRequest(), "other", std::to_string(rowId(first))));
        CHECK(static_cast<int>(x_resp->getStatusCode()) == 404);
    }

    try {
        execSetup(drogon::app().getDbClient(), "DROP TABLE IF EXISTS crud_wire_test");
    } catch (const std::exception&) {
    }
}

} // namespace

int main() {
    // --- Int: integers, decimal strings, hex strings bind canonically ------
    checkBindOk(FieldType::Int, parse("30"), "30");
    checkBindOk(FieldType::Int, parse("-5"), "-5");
    checkBindOk(FieldType::Int, parse("2147483647"), "2147483647");
    checkBindOk(FieldType::Int, parse("-2147483648"), "-2147483648");
    checkBindOk(FieldType::Int, parse("\"42\""), "42");
    checkBindOk(FieldType::Int, parse("\"+7\""), "7");
    checkBindOk(FieldType::Int, parse("\"-7\""), "-7");
    checkBindOk(FieldType::Int, parse("\"0x2A\""), "42");
    checkBindOk(FieldType::Int, parse("\"0XFF\""), "255");
    checkBindOk(FieldType::Int, parse("30.0"), "30");    // integral real: exactly representable, accepted as a number
    checkBindRejected(FieldType::Int, parse("30.5"));    // non-integral real: no exact int form
    checkBindRejected(FieldType::Int, parse("true"));    // boolean is not an int
    checkBindRejected(FieldType::Int, parse("\"abc\"")); // non-numeric string
    checkBindRejected(FieldType::Int, parse("\"\""));    // empty string
    checkBindRejected(FieldType::Int, parse("\" 12\"")); // no whitespace padding
    checkBindRejected(FieldType::Int, parse("\"12 \""));
    checkBindRejected(FieldType::Int, parse("\"0x\"")); // bare prefix
    checkBindRejected(FieldType::Int, parse("null"));
    checkBindRejected(FieldType::Int, parse("{}"));
    checkBindRejected(FieldType::Int, parse("[1]"));
    checkBindRejected(FieldType::Int, parse("2147483648")); // int32 overflow
    checkBindRejected(FieldType::Int, parse("-2147483649"));
    checkBindRejected(FieldType::Int, parse("\"9999999999999999999999\"")); // decimal overflow
    checkBindRejected(FieldType::Int, parse("\"0xFFFFFFFF\""));             // hex overflow for int32

    // --- BigInt: full 64-bit range -----------------------------------------
    checkBindOk(FieldType::BigInt, parse("3000000000"), "3000000000");
    checkBindOk(FieldType::BigInt, parse("-9223372036854775808"), "-9223372036854775808");
    checkBindOk(FieldType::BigInt, parse("9223372036854775807"), "9223372036854775807");
    checkBindOk(FieldType::BigInt, parse("\"0x7FFFFFFFFFFFFFFF\""), "9223372036854775807");
    checkBindRejected(FieldType::BigInt, parse("9223372036854775808")); // uint64 above int64 max
    checkBindRejected(FieldType::BigInt, parse("\"0xFFFFFFFFFFFFFFFF\""));
    checkBindRejected(FieldType::BigInt, parse("1.5"));
    checkBindRejected(FieldType::BigInt, parse("false"));
    checkBindRejected(FieldType::BigInt, parse("\"abc\""));
    checkBindRejected(FieldType::BigInt, parse("null"));

    // --- Text: strings only -------------------------------------------------
    checkBindOk(FieldType::Text, parse("\"hello\""), "hello");
    checkBindOk(FieldType::Text, parse("\"\""), "");
    checkBindRejected(FieldType::Text, parse("30"));
    checkBindRejected(FieldType::Text, parse("30.5"));
    checkBindRejected(FieldType::Text, parse("true"));
    checkBindRejected(FieldType::Text, parse("null"));
    checkBindRejected(FieldType::Text, parse("{}"));

    // --- Bool: booleans only ------------------------------------------------
    checkBindOk(FieldType::Bool, parse("true"), "true");
    checkBindOk(FieldType::Bool, parse("false"), "false");
    checkBindRejected(FieldType::Bool, parse("1"));
    checkBindRejected(FieldType::Bool, parse("0"));
    checkBindRejected(FieldType::Bool, parse("\"true\""));
    checkBindRejected(FieldType::Bool, parse("\"false\""));
    checkBindRejected(FieldType::Bool, parse("null"));

    // --- Timestamp: ISO strings only ----------------------------------------
    checkBindOk(FieldType::Timestamp, parse("\"2024-01-15T10:30:00Z\""), "2024-01-15T10:30:00Z");
    checkBindRejected(FieldType::Timestamp, parse("1705314600")); // epoch number, ambiguous unit
    checkBindRejected(FieldType::Timestamp, parse("1705314600000"));
    checkBindRejected(FieldType::Timestamp, parse("true"));
    checkBindRejected(FieldType::Timestamp, parse("null"));
    checkBindRejected(FieldType::Timestamp, parse("\"\""));
    checkBindRejected(FieldType::Timestamp, parse("{}"));

    // --- Jsonb: every JSON value re-serializes (nothing is rejected) --------
    {
        auto res = fieldValueToBind("col", FieldType::Jsonb, parse("{\"a\":1,\"b\":[1,2]}"));
        CHECK(!res.hasError());
        if (!res.hasError())
            CHECK(parse(res.value()) == parse("{\"a\":1,\"b\":[1,2]}"));
    }
    checkBindOk(FieldType::Jsonb, parse("[1,2]"), "[1,2]");
    checkBindOk(FieldType::Jsonb, parse("\"x\""), "\"x\"");
    checkBindOk(FieldType::Jsonb, parse("5"), "5");
    checkBindOk(FieldType::Jsonb, parse("true"), "true");
    checkBindOk(FieldType::Jsonb, parse("null"), "null");

    // --- HTTP level: mismatches are 400 on insert AND update -----------------
    // (No database is configured; validation runs before any DB access, so
    // these return 400 without one. A valid body would proceed to the DB
    // stage instead — covered by the helper checks above.)
    checkHttpRejected("{\"c_int\":\"abc\"}");
    checkHttpRejected("{\"c_int\":30.5}");
    checkHttpRejected("{\"c_int\":true}");
    checkHttpRejected("{\"c_int\":2147483648}");
    checkHttpRejected("{\"c_bigint\":\"0xFFFFFFFFFFFFFFFF\"}");
    checkHttpRejected("{\"c_text\":30}");
    checkHttpRejected("{\"c_text\":true}");
    checkHttpRejected("{\"c_bool\":\"yes\"}");
    checkHttpRejected("{\"c_bool\":1}");
    checkHttpRejected("{\"c_ts\":1705314600}");
    checkHttpRejected("{\"c_ts\":null}");

    // A fully valid body clears validation (it reaches the DB stage, which
    // fails without a configured database — anything but 400 proves the
    // values were accepted).
    {
        const Json::Value body = parse("{\"c_int\":30,\"c_bigint\":\"0x2A\",\"c_text\":\"hi\",\"c_bool\":true,"
                                       "\"c_ts\":\"2024-01-15T10:30:00Z\",\"c_jsonb\":{\"a\":1}}");
        CHECK(postStatus(body) != 400);
        CHECK(patchStatus(body) != 400);
    }

    // --- HTTP level: wire-format cases (live DB when reachable, else SKIP) --
    {
        const std::string pg_host = dbEnv("PGHOST", "127.0.0.1");
        const int pg_port = dbPort();
        if (!tcpReachable(pg_host, pg_port)) {
            std::printf(
                "crud_binds_test: SKIP wire-format section (no postgres at %s:%d; set PGHOST/PGPORT/PGDATABASE/PGUSER/PGPASSWORD)\n",
                pg_host.c_str(), pg_port);
        } else {
            try {
                drogon::orm::PostgresConfig cfg;
                cfg.host = pg_host;
                cfg.port = static_cast<unsigned short>(pg_port);
                cfg.databaseName = dbEnv("PGDATABASE", "sgrn");
                cfg.username = dbEnv("PGUSER", "sgrn_datastore");
                cfg.password = dbEnv("PGPASSWORD", "dracaeris");
                cfg.connectionNumber = 1;
                cfg.name = "default";
                cfg.isFast = false;
                cfg.timeout = 5.0;
                cfg.autoBatch = false;
                drogon::app().addDbClient(cfg);
                // The engine resolves its client through the app singleton,
                // whose clients only materialize inside run(). Detach the
                // loop thread: the binary _exit()s at the end (see below),
                // which reaps it — no quit/join, no shutdown races.
                std::thread loop([] { drogon::app().run(); });
                loop.detach();
                // Bounded wait for the framework + a live connection. Without
                // this, getDbClient() races run()'s createDbClients and
                // reads null.
                drogon::orm::DbClientPtr client;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                while (std::chrono::steady_clock::now() < deadline) {
                    if (drogon::app().isRunning()) {
                        client = drogon::app().getDbClient();
                        if (client && client->hasAvailableConnections()) {
                            break;
                        }
                        client.reset();
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (client) {
                    runWireFormatTests();
                } else {
                    std::printf("crud_binds_test: SKIP wire-format section (no usable connection at %s:%d)\n", pg_host.c_str(), pg_port);
                }
            } catch (const std::exception& e) {
                std::printf("crud_binds_test: SKIP wire-format section (harness failed: %s)\n", e.what());
            }
        }
    }

    if (g_failures == 0)
        std::printf("crud_binds_test: ALL CHECKS PASSED\n");
    else
        std::printf("crud_binds_test: %d FAILURES\n", g_failures);

    // NOTE: _exit(), not return. The valid-body cases above reach
    // drogon::app().getDbClient(), which instantiates the framework
    // singleton; its static teardown segfaults when the event loop never ran
    // (production always runs it, so this is test-only). Flush first so no
    // result is lost, then bypass static destructors with the exit code.
    std::fflush(stdout);
    _exit(g_failures == 0 ? 0 : 1);
}
