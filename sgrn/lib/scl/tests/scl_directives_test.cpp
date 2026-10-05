// scl_directives_test.cpp — parser coverage for the semantic directives:
// #DIMENSIONS/#DIMENSION (+undeclared-dimension rejection), #DESC, #LABEL,
// #PRECISION, #NOMINAL, #TRANSIENT, #READ_ONLY, #ALARM, and scalar-alias
// inheritance of the display metadata (unit/dimension/desc/precision/
// nominal/alarm) but NOT of the per-signal roles (label/transient/read-only).
#include <sgrn/scl/schema/DbSymbolsParser.hpp>

#include <cassert>
#include <cstdio>
#include <string>

using namespace sgrn::scl;

static int g_failures = 0;
#define CHECK(cond)                                                                                                                        \
    do {                                                                                                                                   \
        if (!(cond)) {                                                                                                                     \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                                    \
        }                                                                                                                                  \
    } while (0)

static const DbField* findField(const std::vector<DbField>& t_fields, const std::string& t_name) {
    for (const auto& f : t_fields) {
        if (f.name == t_name)
            return &f;
    }
    return nullptr;
}

static const char* kSchema = R"(
#DIMENSIONS("pressure", "temperature", "flow");

TYPE "Press" : Real #UNIT("kPa") #DIMENSION("pressure") #DESC("Pressure type") #PRECISION(1) #NOMINAL(2700.0) END_TYPE

DATA_BLOCK "Plant" DB 1
VAR
    reactor_pressure : "Press" #ALARM(2500.0, 3000.0);
    reactor_temp     : Real #UNIT("degC") #DIMENSION("temperature") #DESC("Reactor temp") #PRECISION(2) #NOMINAL(120.0);
    feed_flow        : Real #UNIT("kscmh") #DIMENSION("flow");
    "fault kein"     : Int #LABEL;
    skip_me          : Real #TRANSIENT;
    lock_me          : Real #READ_ONLY;
END_VAR
END_DATA_BLOCK
)";

int main() {
    auto res = DbSymbolsParser::parseString(kSchema);
    CHECK(!res.hasError());
    if (res.hasError())
        return 1;
    const ParseResult& r = res.value();
    CHECK(r.dimensions.size() == 3);

    const DbSchema* db = nullptr;
    for (const auto& d : r.dbs) {
        if (d.db_name == "Plant")
            db = &d;
    }
    CHECK(db != nullptr);
    if (!db)
        return 1;

    const DbField* p = findField(db->fields, "reactor_pressure");
    CHECK(p && p->unit && *p->unit == "kPa"); // inherited from alias
    CHECK(p && p->dimension && *p->dimension == "pressure");
    CHECK(p && p->description && *p->description == "Pressure type");
    CHECK(p && p->precision && *p->precision == 1);
    CHECK(p && p->nominal && *p->nominal == 2700.0);
    CHECK(p && p->alarm_lo && *p->alarm_lo == 2500.0);
    CHECK(p && p->alarm_hi && *p->alarm_hi == 3000.0);
    CHECK(p && !p->is_label);

    const DbField* t = findField(db->fields, "reactor_temp");
    CHECK(t && t->description && *t->description == "Reactor temp");
    CHECK(t && t->precision && *t->precision == 2);
    CHECK(t && t->nominal && *t->nominal == 120.0);
    CHECK(t && !t->alarm_lo.has_value());

    const DbField* f = findField(db->fields, "feed_flow");
    CHECK(f && f->dimension && *f->dimension == "flow");
    CHECK(f && !f->description.has_value());

    const DbField* lbl = findField(db->fields, "fault kein");
    CHECK(lbl && lbl->is_label);
    CHECK(lbl && !lbl->is_transient && !lbl->is_read_only);

    const DbField* tr = findField(db->fields, "skip_me");
    CHECK(tr && tr->is_transient && !tr->is_label);
    const DbField* ro = findField(db->fields, "lock_me");
    CHECK(ro && ro->is_read_only && !ro->is_label);

    // JSON round-trip keeps the new metadata.
    const std::string json = toJsonString(*p);
    CHECK(json.find("\"dimension\"") != std::string::npos);
    CHECK(json.find("\"alarm_lo\"") != std::string::npos);
    CHECK(json.find("\"precision\"") != std::string::npos);

    // Undeclared dimension is rejected when a vocabulary exists.
    auto bad = DbSymbolsParser::parseString(
        "#DIMENSIONS(\"pressure\");\nDATA_BLOCK \"B\" DB 2\nVAR\n x : Real #DIMENSION(\"nope\");\nEND_VAR\nEND_DATA_BLOCK\n");
    CHECK(bad.hasError());

    // Without a vocabulary, any dimension is accepted.
    auto free = DbSymbolsParser::parseString("DATA_BLOCK \"B\" DB 2\nVAR\n x : Real #DIMENSION(\"nope\");\nEND_VAR\nEND_DATA_BLOCK\n");
    CHECK(!free.hasError());

    // Bad precision / inverted alarm are rejected.
    auto bad_prec = DbSymbolsParser::parseString("DATA_BLOCK \"B\" DB 2\nVAR\n x : Real #PRECISION(99);\nEND_VAR\nEND_DATA_BLOCK\n");
    CHECK(bad_prec.hasError());
    auto bad_alarm = DbSymbolsParser::parseString("DATA_BLOCK \"B\" DB 2\nVAR\n x : Real #ALARM(5.0, 1.0);\nEND_VAR\nEND_DATA_BLOCK\n");
    CHECK(bad_alarm.hasError());

    if (g_failures == 0)
        std::printf("scl_directives_test: ALL CHECKS PASSED\n");
    else
        std::printf("scl_directives_test: %d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
