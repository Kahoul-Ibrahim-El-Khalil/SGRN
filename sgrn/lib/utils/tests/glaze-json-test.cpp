// sgrn::json (Glaze wrapper) smoke test.
// Exercises: round-trip, std::optional, std::vector, nested struct,
// unknown-key tolerance, wrong-type error, and RawJson splicing.
//
// Run with: glaze_json_test  (exit 0 = pass, non-zero = fail)
#include <sgrn/utils/glaze.hpp>

#include <cassert>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

// ── Types under test ──────────────────────────────────────────────────────────

struct Inner {
    int x{};
    std::string label;
};

template <>
struct glz::meta<Inner> {
    using T = Inner;
    static constexpr auto value = glz::object("x", &T::x, "label", &T::label);
};

struct Outer {
    std::string name;
    std::optional<std::string> note;
    std::vector<int> ids;
    Inner child;
};

template <>
struct glz::meta<Outer> {
    using T = Outer;
    static constexpr auto value = glz::object("name", &T::name, "note", &T::note, "ids", &T::ids, "child", &T::child);
};

// A struct that wraps a raw JSON fragment for splicing.
struct WithRaw {
    std::string key;
    glz::raw_json value; // pre-formatted number or "null"
};

template <>
struct glz::meta<WithRaw> {
    using T = WithRaw;
    static constexpr auto value = glz::object("key", &T::key, "value", &T::value);
};

// ── Helpers ───────────────────────────────────────────────────────────────────

static void check(bool t_cond, const char* t_msg) {
    if (!t_cond) {
        std::cerr << "FAIL: " << t_msg << '\n';
        std::exit(1);
    }
}

// ── Tests ─────────────────────────────────────────────────────────────────────

static void test_round_trip() {
    Outer orig;
    orig.name = "reactor";
    orig.note = "test note";
    orig.ids = {1, 2, 3};
    orig.child = {42, "sub"};

    auto ws = sgrn::json::write(orig);
    check(!ws.hasError(), "write should succeed");

    auto rs = sgrn::json::read<Outer>(ws.value());
    check(!rs.hasError(), "read should succeed");

    const Outer& back = rs.value();
    check(back.name == orig.name, "name round-trips");
    check(back.note == orig.note, "optional note round-trips");
    check(back.ids == orig.ids, "vector ids round-trips");
    check(back.child.x == 42, "child.x round-trips");
    check(back.child.label == "sub", "child.label round-trips");

    std::puts("PASS test_round_trip");
}

static void test_null_optional() {
    Outer orig;
    orig.name = "empty";
    // note is nullopt

    auto ws = sgrn::json::write(orig);
    check(!ws.hasError(), "write with nullopt should succeed");
    // With kWriteOpts.skip_null_members = false, "note":null must be present.
    check(ws.value().find("\"note\":null") != std::string::npos, "null optional must appear in output");

    auto rs = sgrn::json::read<Outer>(ws.value());
    check(!rs.hasError(), "read back should succeed");
    check(!rs.value().note.has_value(), "note should still be nullopt");
    std::puts("PASS test_null_optional");
}

static void test_unknown_key_tolerance() {
    // JSON has an extra "extra_field" key — must not fail.
    std::string_view json = R"({"name":"x","extra_field":99,"ids":[],"child":{"x":0,"label":""}})";
    auto rs = sgrn::json::read<Outer>(json);
    check(!rs.hasError(), "unknown key must be tolerated");
    check(rs.value().name == "x", "name read correctly despite unknown key");
    std::puts("PASS test_unknown_key_tolerance");
}

static void test_wrong_type_error() {
    // "ids" should be an array but we give a string.
    std::string_view json = R"({"name":"x","ids":"not_array","child":{"x":0,"label":""}})";
    auto rs = sgrn::json::read<Outer>(json);
    check(rs.hasError(), "wrong type must produce an error");
    check(!rs.error().message.empty(), "error message must not be empty");
    std::puts("PASS test_wrong_type_error");
}

static void test_raw_json_splice() {
    WithRaw wr;
    wr.key = "speed";
    wr.value = glz::raw_json{"1.2345678e-05"}; // pre-formatted REAL

    auto ws = sgrn::json::write(wr);
    check(!ws.hasError(), "write WithRaw should succeed");
    // The raw fragment must appear verbatim (not re-encoded as a string).
    check(ws.value().find("1.2345678e-05") != std::string::npos, "raw JSON value must appear verbatim");
    std::puts("PASS test_raw_json_splice");
}

static void test_null_raw_json() {
    // Simulate NaN → "null" via raw_json
    WithRaw wr;
    wr.key = "temp";
    wr.value = glz::raw_json{"null"};

    auto ws = sgrn::json::write(wr);
    check(!ws.hasError(), "write null raw_json should succeed");
    check(ws.value().find("\"value\":null") != std::string::npos, "null raw_json must appear as JSON null");
    std::puts("PASS test_null_raw_json");
}

static void test_pretty_write() {
    Inner i{7, "pretty"};
    auto ws = sgrn::json::writePretty(i);
    check(!ws.hasError(), "writePretty should succeed");
    // Pretty output must contain newlines.
    check(ws.value().find('\n') != std::string::npos, "pretty output must contain newlines");
    std::puts("PASS test_pretty_write");
}

static void test_writeTo_clears() {
    Inner i{1, "a"};
    std::string buf = "old content";
    auto r = sgrn::json::writeTo(buf, i);
    check(!r.hasError(), "writeTo should succeed");
    check(buf.find("old content") == std::string::npos, "writeTo must clear previous content");
    std::puts("PASS test_writeTo_clears");
}

static void test_dynamic_value() {
    std::string_view json = R"({"db":10,"tag":"speed","value":1.5})";
    auto rs = sgrn::json::parseValue(json);
    check(!rs.hasError(), "parseValue should succeed");
    // Check it is an object.
    check(std::holds_alternative<glz::json_t::object_t>(rs.value().data), "top-level value must be an object");
    std::puts("PASS test_dynamic_value");
}

// ─────────────────────────────────────────────────────────────────────────────

int main() {
    test_round_trip();
    test_null_optional();
    test_unknown_key_tolerance();
    test_wrong_type_error();
    test_raw_json_splice();
    test_null_raw_json();
    test_pretty_write();
    test_writeTo_clears();
    test_dynamic_value();

    std::puts("All sgrn::json smoke tests passed.");
    return 0;
}
