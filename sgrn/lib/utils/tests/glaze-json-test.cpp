// sgrn::json (Glaze wrapper) smoke test.
// Exercises: round-trip, std::optional, std::vector, nested struct,
// unknown-key tolerance, wrong-type error, and RawJson splicing.
//
// Run with: glaze_json_test  (exit 0 = pass, non-zero = fail)
#include <sgrn/utils/json-dom.hpp>
#include <sgrn/utils/json-ops.hpp>
#include <sgrn/utils/json-serialization.hpp>

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

static void testRoundTrip() {
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

    std::puts("PASS testRoundTrip");
}

static void testNullOptional() {
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
    std::puts("PASS testNullOptional");
}

static void testUnkownKeyTolerance() {
    // JSON has an extra "extra_field" key — must not fail.
    std::string_view json = R"({"name":"x","extra_field":99,"ids":[],"child":{"x":0,"label":""}})";
    auto rs = sgrn::json::read<Outer>(json);
    check(!rs.hasError(), "unknown key must be tolerated");
    check(rs.value().name == "x", "name read correctly despite unknown key");
    std::puts("PASS testUnkownKeyTolerance");
}

static void testWrongTypeError() {
    // "ids" should be an array but we give a string.
    std::string_view json = R"({"name":"x","ids":"not_array","child":{"x":0,"label":""}})";
    auto rs = sgrn::json::read<Outer>(json);
    check(rs.hasError(), "wrong type must produce an error");
    check(!rs.error().message.empty(), "error message must not be empty");
    std::puts("PASS testWrongTypeError");
}

static void testDynamicDomHelpers() {
    auto parsed = sgrn::json::parseValue(R"({"id":42,"items":[1,2],"enabled":true})");
    check(!parsed.hasError(), "dynamic object parses");

    auto& root = parsed.value();
    check(root.is_object(), "dynamic root is an object");
    check(sgrn::json::contains(root, "id"), "contains finds existing member");
    check(!sgrn::json::contains(root, "missing"), "contains rejects missing member");

    const auto* id = sgrn::json::find(root, "id");
    check(id && id->is_int64() && id->get<int64_t>() == 42, "find preserves signed integer value");

    sgrn::json::insertOrAssign(root, "name", sgrn::json::string("gateway"));
    check(sgrn::json::erase(root, "enabled"), "erase removes an existing member");
    check(!sgrn::json::erase(root, "enabled"), "erase reports an absent member");

    auto* items = sgrn::json::find(root, "items");
    check(items && items->is_array(), "find returns the array member");
    sgrn::json::append(*items, sgrn::json::integer(3));

    auto encoded = sgrn::json::write(root);
    check(!encoded.hasError(), "mutated dynamic value serializes");
    auto round_trip = sgrn::json::parseValue(encoded.value());
    check(!round_trip.hasError(), "mutated dynamic value parses again");
    check(sgrn::json::find(round_trip.value(), "name")->get<std::string>() == "gateway", "inserted string round-trips");
    check(sgrn::json::find(round_trip.value(), "items")->size() == 3, "array append round-trips");

    auto created = sgrn::json::object();
    sgrn::json::insertOrAssign(created, "ok", sgrn::json::boolean(true));
    check(sgrn::json::find(created, "ok")->get<bool>(), "empty object builder creates a mutable object");
    auto created_array = sgrn::json::array();
    sgrn::json::append(created_array, sgrn::json::number(1.5));
    check(created_array.is_array() && created_array.size() == 1, "empty array builder creates a mutable array");
    std::puts("PASS testDynamicDomHelpers");
}

static void testJsonCcomments() {
    constexpr std::string_view jsonc = R"({
        // device metadata
        "device": "pump-1",
        /* state */
        "enabled": true
    })";
    auto parsed = sgrn::json::parseJsoncValue(jsonc);
    check(!parsed.hasError(), "JSONC comments are accepted");
    check(parsed.value().is_object(), "JSONC root is an object");
    check(sgrn::json::find(parsed.value(), "device")->get<std::string>() == "pump-1", "JSONC member is readable");
    std::puts("PASS testJsonCcomments");
}

static void testGlazeDomCompatibility() {
    using namespace sgrn::json::dom;

    Document doc;
    constexpr unsigned relaxed = kParseCommentsFlag | kParseTrailingCommasFlag;
    doc.parse<relaxed>(R"({
        // root comment
        "nested": {"items": [1, 2,],},
    })");
    check(!doc.hasParseError() && doc.is_object(), "Glaze DOM parses comments and trailing commas");
    Document malformed;
    malformed.parse<relaxed>(R"({"value": 1} /* unterminated)");
    check(malformed.hasParseError(), "unterminated block comments are rejected");

    Value nested = doc["nested"];
    Value items = nested["items"];
    check(items.is_array() && items.size() == 2, "DOM array member is addressable through a view");
    int array_sum = 0;
    for (const Value item : items.elements())
        array_sum += item.getInt();
    check(array_sum == 3, "DOM array range iterates values without copying the owning tree");
    check(doc.memberCount() == 1 && doc.findMember("nested") != doc.memberEnd(), "DOM member lookup and count work");
    items[1].setInt(9);
    check(doc["nested"]["items"][1].getInt() == 9, "mutating a child view updates the owning document");
    Document expected;
    expected.parse(R"({"nested":{"items":[1,9]}})");
    check(doc == expected, "DOM equality compares nested objects and arrays recursively");

    bool missing_throws = false;
    try {
        (void)doc["missing"];
    } catch (const std::out_of_range&) {
        missing_throws = true;
    }
    check(missing_throws && !doc.hasMember("missing"), "DOM lookup never inserts missing members");

    StringBuffer compact;
    Writer<StringBuffer> writer(compact);
    doc.accept(writer);
    Document round_trip;
    round_trip.parse(compact.getString(), compact.size());
    check(!round_trip.hasParseError() && round_trip["nested"]["items"][1].getInt() == 9, "DOM writer output round-trips through Glaze");
    doc["nested"]["items"][1].accept(writer);
    check(std::string_view(compact.getString()) == "9", "DOM writer can be reused without retaining a previous root");

    StringBuffer pretty;
    PrettyWriter<StringBuffer> pretty_writer(pretty);
    pretty_writer.setIndent(' ', 2);
    doc.accept(pretty_writer);
    check(std::string_view(pretty.getString()).find("\n  \"nested\"") != std::string_view::npos,
        "DOM pretty writer honors two-space indentation");
    std::puts("PASS testGlazeDomCompatibility");
}

static void testJsonUtilityOps() {
    auto parsed = sgrn::utils::json::parse(R"({"DB1":{"Motor":{"speed":12,"enabled":true}},"ignored":1})");
    check(!parsed.hasError(), "JSON utility parse accepts an object root");
    auto subtree = sgrn::utils::json::extractSubtree(R"({"DB1":{"Motor":{"speed":12}}})", "db1/Motor");
    check(subtree.has_value() && subtree->find("speed") != std::string::npos,
        "case-insensitive subtree extraction preserves the selected JSON value");
    const auto filtered = sgrn::utils::json::filterFields(parsed.value(), {"DB1.Motor.speed"});
    check(filtered.find("speed") != std::string::npos && filtered.find("enabled") == std::string::npos &&
              filtered.find("ignored") == std::string::npos,
        "field filtering retains only the requested nested leaf");
    check(sgrn::utils::json::serializeValue("key", "value") == R"({"key":"value"})", "key/value serializer emits compact JSON");
    std::puts("PASS testJsonUtilityOps");
}

static void testJsonRawSplice() {
    WithRaw wr;
    wr.key = "speed";
    wr.value = glz::raw_json{"1.2345678e-05"}; // pre-formatted REAL

    auto ws = sgrn::json::write(wr);
    check(!ws.hasError(), "write WithRaw should succeed");
    // The raw fragment must appear verbatim (not re-encoded as a string).
    check(ws.value().find("1.2345678e-05") != std::string::npos, "raw JSON value must appear verbatim");
    std::puts("PASS testJsonRawSplice");
}

static void testNullRawJson() {
    // Simulate NaN → "null" via raw_json
    WithRaw wr;
    wr.key = "temp";
    wr.value = glz::raw_json{"null"};

    auto ws = sgrn::json::write(wr);
    check(!ws.hasError(), "write null raw_json should succeed");
    check(ws.value().find("\"value\":null") != std::string::npos, "null raw_json must appear as JSON null");
    std::puts("PASS testNullRawJson");
}

static void testPrettyWrite() {
    Inner i{7, "pretty"};
    auto ws = sgrn::json::writePretty(i);
    check(!ws.hasError(), "writePretty should succeed");
    // Pretty output must contain newlines.
    check(ws.value().find('\n') != std::string::npos, "pretty output must contain newlines");
    std::puts("PASS testPrettyWrite");
}

static void testWriteToClear() {
    Inner i{1, "a"};
    std::string buf = "old content";
    auto r = sgrn::json::writeTo(buf, i);
    check(!r.hasError(), "writeTo should succeed");
    check(buf.find("old content") == std::string::npos, "writeTo must clear previous content");
    std::puts("PASS testWriteToClear");
}

static void testDynamicValue() {
    std::string_view json = R"({"db":10,"tag":"speed","value":1.5})";

    auto rs = sgrn::json::parseValue(json);

    check(!rs.hasError(), "parseValue should succeed");

    // Check it is an object.

    check(std::holds_alternative<sgrn::json::Value::object_t>(rs.value().data), "top-level value must be an object");

    std::puts("PASS testDynamicValue");
}

// ─────────────────────────────────────────────────────────────────────────────

int main() {
    testRoundTrip();
    testNullOptional();
    testUnkownKeyTolerance();
    testWrongTypeError();
    testDynamicDomHelpers();
    testGlazeDomCompatibility();
    testJsonUtilityOps();
    testJsonCcomments();
    testJsonRawSplice();
    testNullRawJson();
    testPrettyWrite();
    testWriteToClear();
    testDynamicValue();

    std::puts("All sgrn::json smoke tests passed.");
    return 0;
}
