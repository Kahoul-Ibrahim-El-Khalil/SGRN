#pragma once
#include <fmt/color.h>
#include <fmt/format.h>
#include <sgrn/Result.hpp>
#include <sgrn/gateway/twin/PlcCommandProcessor.hpp>
#include <sgrn/s7shell/connection/S7Connection.hpp>
#include <sgrn/s7shell/script/ScriptDataBlock.hpp>
#include <sgrn/s7shell/utils/PlcSimClock.hpp>
#include <sgrn/scl/types.hpp>
#include <angelscript.h>
#include <ctime>
#include <s7codec/codec.hpp>
#include <snap7.h>
#include <string>
#include <string_view>

class asIScriptEngine;

namespace sgrn::s7shell::shell
{

::sgrn::Result<void, std::string> registerS7Shell(asIScriptEngine* tp_engine);
namespace scl = ::sgrn::scl;

namespace detail
{
// The value-returning checks themselves — renamed out of the way of the
// SGRN_CHECK_AS macro below. [[nodiscard]] is defense in depth: if anyone
// calls these directly instead of through the macro, the compiler flags it
// instead of silently swallowing a failed registration like before.
[[nodiscard]] inline ::sgrn::Result<void, std::string> checkAsResult(int t_r, std::string_view t_ctx) {
    if (t_r < 0) {
        fmt::print(stderr, fg(fmt::color::red), "AngelScript registration failed [{}]: code {}\n", t_ctx, t_r);
        return std::string(t_ctx);
    }
    return {};
}

[[nodiscard]] inline ::sgrn::Result<void, std::string> checkAsResult(
    int t_r, std::string_view t_ctx, int t_behavior, std::string_view t_decl) {
    if (t_r < 0) {
        fmt::print(stderr, fg(fmt::color::red), "AngelScript registration failed [{}]: code {}, decl{} \n", t_ctx, t_r, t_decl);
        return std::string(t_ctx);
    }
    return {};
}
} // namespace detail

// SGRN_CHECK_AS must be a macro, not a function: on failure it needs to
// `return` out of *whatever RegisterXXX() called it*, immediately, before
// registering anything else against a partially-broken AngelScript engine.
// A function can only report failure through its own return value — and
// every call site in this codebase discarded that value, so a failed
// engine->RegisterXXX(...) call was logged and then ignored, letting the
// enclosing RegisterXXX() report success regardless. Expanding to an early
// `return` here fixes every existing call site with no change to the call
// site itself.
//
// Requires: the enclosing function returns ::sgrn::Result<void, std::string>
// (true of every RegisterXXX() in src/bindings/*.cpp).
#define SGRN_CHECK_AS_RES(...)                                                                                                             \
    do {                                                                                                                                   \
        if (auto _sgrn_check_as_res = ::sgrn::s7shell::shell::detail::checkAsResult(__VA_ARGS__); _sgrn_check_as_res.hasError()) {         \
            return _sgrn_check_as_res;                                                                                                     \
        }                                                                                                                                  \
    } while (0)

#define SGRN_CHECK_AS_RESULT(r) SGRN_CHECK_AS_RES(r, "AngelScript")
#define SGRN_REGISTER_AS_BINDING(r, expr)                                                                                                  \
    do {                                                                                                                                   \
        r = expr;                                                                                                                          \
        SGRN_CHECK_AS_RESULT(r);                                                                                                           \
    } while (0)

// ─────────────────────────────────────────────────────────────────────────────
// Verbosity-reduction macros for src/bindings/*.cpp
//
// Every RegisterXXX() in that directory follows the same shape: an `int r`
// declared once at the top of the function, then a long run of
//   r = tp_engine->RegisterYYY(...);
//   SGRN_CHECK_AS_RESULT(r);
// pairs. SGRN_AS_REG collapses that pair to a single statement built on top
// of SGRN_REGISTER_AS_BINDING above. Two even more common shapes get their
// own macros on top of that:
//   - SGRN_AS_TYPE       registers a ref-counted AS object type.
//   - SGRN_AS_REFCOUNTED registers the addref/release behaviour pair every
//                        ref-counted AS type needs.
// All three still require the enclosing function to have a local `int r`
// and to return ::sgrn::Result<void, std::string>, exactly like
// SGRN_REGISTER_AS_BINDING itself.
// ─────────────────────────────────────────────────────────────────────────────
#define SGRN_AS_REG(expr) SGRN_REGISTER_AS_BINDING(r, (expr))

#define SGRN_AS_TYPE(engine, name) SGRN_AS_REG((engine)->RegisterObjectType(name, 0, asOBJ_REF))

#define SGRN_AS_REFCOUNTED(engine, name, cls)                                                                                              \
    do {                                                                                                                                   \
        SGRN_AS_REG((engine)->RegisterObjectBehaviour(name, asBEHAVE_ADDREF, "void f()", asMETHOD(cls, addRef), asCALL_THISCALL));         \
        SGRN_AS_REG((engine)->RegisterObjectBehaviour(name, asBEHAVE_RELEASE, "void f()", asMETHOD(cls, release), asCALL_THISCALL));       \
    } while (0)

// Collapses the `if (auto res = registerXXX(engine); res.hasError()) return
// res;` chain in registerS7Shell() (registration.cpp) to one line per module.
#define SGRN_REGISTER_MODULE(expr)                                                                                                         \
    do {                                                                                                                                   \
        if (auto _sgrn_mod_res = (expr); _sgrn_mod_res.hasError())                                                                         \
            return _sgrn_mod_res;                                                                                                          \
    } while (0)

inline std::string dtlToString(const s7codec::S7RawDTL& t_d) {
    char buf[40];
    // Format must match encodeScalar DTL sscanf: "YYYY-MM-DD HH:MM:SS.nnnnnnnnn"
    // (space separator, 9-digit nanoseconds)
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u.%09u", t_d.year, t_d.month, t_d.day, t_d.hour, t_d.minute, t_d.second,
        t_d.nanosecond);
    return buf;
}

inline s7codec::S7RawDTL dtlFromTm(const std::tm& t_ti) {
    s7codec::S7RawDTL t_d;
    t_d.year = static_cast<uint16_t>(t_ti.tm_year + 1900);
    t_d.month = static_cast<uint8_t>(t_ti.tm_mon + 1);
    t_d.day = static_cast<uint8_t>(t_ti.tm_mday);
    t_d.weekday = static_cast<uint8_t>(t_ti.tm_wday + 1);
    t_d.hour = static_cast<uint8_t>(t_ti.tm_hour);
    t_d.minute = static_cast<uint8_t>(t_ti.tm_min);
    t_d.second = static_cast<uint8_t>(t_ti.tm_sec);
    t_d.nanosecond = 0;
    return t_d;
}

inline ScriptDtl* makeScriptDtlFromClock() {
    s7codec::S7RawDTL t_d = dtlFromTm(g_plc_clock.nowLocalTm());
    t_d.nanosecond = static_cast<uint32_t>((g_plc_clock.nowMs() % 1000) * 1000000);
    auto* p_obj = new ScriptDtl();
    p_obj->timestamp_str_ = dtlToString(t_d); // raw string, no quotes
    return p_obj;
}

inline ScriptDtl* ScriptDtl_now() {
    return makeScriptDtlFromClock();
}

inline ScriptDtl* ScriptDtl_fromString(const std::string& t_s) {
    auto* p_obj = new ScriptDtl();
    p_obj->timestamp_str_ = t_s; // store raw, toString() adds quotes
    return p_obj;
}

inline std::string get_dtl_now() {
    s7codec::S7RawDTL dtl = dtlFromTm(g_plc_clock.nowLocalTm());
    dtl.nanosecond = static_cast<uint32_t>((g_plc_clock.nowMs() % 1000) * 1000000);
    return "\"" + dtlToString(dtl) + "\"";
}

inline void Script_setPlcTime(int t_year, int t_month, int t_day, int t_hour, int t_minute, int t_second) {
    g_plc_clock.setLocal(t_year, t_month, t_day, t_hour, t_minute, t_second);
}

inline void Script_advancePlcTime(int t_delta_ms) {
    g_plc_clock.advanceMs(t_delta_ms);
}

inline void Script_resetPlcTime() {
    g_plc_clock.useWallClock();
}

inline void printFloat(float t_v) {
    fmt::print("{}\n", t_v);
}
inline void printDouble(double t_v) {
    fmt::print("{}\n", t_v);
}
inline void printInt(int t_v) {
    fmt::print("{}\n", t_v);
}
inline void printUInt(unsigned int t_v) {
    fmt::print("{}\n", t_v);
}
inline void printBool(bool t_v) {
    fmt::print("{}\n", t_v ? "true" : "false");
}
inline void printInt64(int64_t t_v) {
    fmt::print("{}\n", t_v);
}
inline void printUInt64(uint64_t t_v) {
    fmt::print("{}\n", t_v);
}

inline ScriptS7Client* S7Client_factory(const std::string& t_ip, int t_rack, int t_slot, uint16_t t_port) {
    return new ScriptS7Client(t_ip, t_rack, t_slot, t_port);
}

inline ScriptS7Client* S7Client_factoryWithSchema(
    const std::string& t_ip, int t_rack, int t_slot, uint16_t t_port, const std::string& t_schema_path) {
    auto* p_client = new ScriptS7Client(t_ip, t_rack, t_slot, t_port);
    p_client->loadSclSchema(t_schema_path);
    return p_client;
}

::sgrn::Result<void, std::string> registerS7Types(asIScriptEngine* tp_engine);
::sgrn::Result<void, std::string> registerS7Globals(asIScriptEngine* tp_engine);

class PlcRuntimeWrapper {
public:
    explicit PlcRuntimeWrapper(std::shared_ptr<::sgrn::s7shell::runtime::PlcRuntime> tsp_impl)
        : impl_(std::move(tsp_impl)) {
    }

    void addRef() {
        ref_count_++;
    }
    void release() {
        if (--ref_count_ == 0)
            delete this;
    }

    void loadSclSchema(const std::string& t_path) {
        impl_->loadSclSchema(t_path);
    }
    void loadJsonSchema(const std::string& t_path) {
        impl_->loadJsonSchema(t_path);
    }
    void registerDb(uint16_t t_num, uint32_t t_size, const std::string& t_name) {
        impl_->registerDb(t_num, t_size, t_name);
    }
    void registerUdt(const std::string& t_name, uint32_t t_size) {
        impl_->registerUdt(t_name, t_size);
    }
    void addUdtField(
        const std::string& t_udt_name, const std::string& t_name, const std::string& t_type_str, uint32_t t_offset, uint16_t t_count) {
        impl_->addUdtField(t_udt_name, t_name, t_type_str, t_offset, t_count);
    }
    void loadRegistry(const std::string& t_path) {
        impl_->loadRegistry(t_path);
    }
    bool hasDirty(uint16_t t_db_num) const {
        return impl_->hasDirty(t_db_num);
    }

    // ── Virtual PLC memory manipulation ────────────────────────────────────
    // Scripts can read/write the shared PlcMemory directly, making the
    // runtime behave as a virtual PLC. An S7Server attached to the same
    // runtime will serve these values to any connecting S7 client.

    /// Write a field by symbolic path. value_json must be a JSON scalar:
    ///   rt.set(1, "pump_1.running", "true")
    ///   rt.set(1, "setpoint",       "42.5")
    bool set(uint16_t t_db, const std::string& t_path, const std::string& t_value_json) {
        auto res = impl_->getMemory().updateField(t_db, t_path, t_value_json);
        if (res.hasError()) {
            fmt::print(stderr, fg(fmt::color::red), "[PlcRuntime] set(DB{}, \"{}\") failed: {}\n", t_db, t_path, res.error());
            return false;
        }
        if (auto db_res = impl_->getSchema().getDb(t_db); !db_res.hasError())
            impl_->markDirty(t_db, 0, static_cast<uint32_t>(db_res.value()->size_bytes));
        // `updateField` only enqueues a PlcCommand::WriteField; without a
        // flush the value never reaches the in-memory arena, so getField() /
        // getJson() still return the old data while set() reports success.
        // The gateway's HTTP/OPC-UA handlers and ScriptDataBlock call
        // processCommands() explicitly — mirror that here so the virtual-PLC
        // write is applied synchronously (no background io_context in shell).
        impl_->getMemory().processor()->processCommands();
        return true;
    }

    /// Read a field by symbolic path. Returns the JSON value as a string, or
    /// an empty string on error (with an error message printed to stderr).
    std::string get(uint16_t t_db, const std::string& t_path) const {
        auto res = impl_->getMemory().getFieldValue(t_db, t_path);
        if (res.hasError()) {
            fmt::print(stderr, fg(fmt::color::red), "[PlcRuntime] get(DB{}, \"{}\") failed: {}\n", t_db, t_path, res.error());
            return {};
        }
        return res.value();
    }

    /// Dump the entire DB as a JSON string.
    std::string getJson(uint16_t t_db) const {
        auto res = impl_->getMemory().getDbJson(t_db);
        if (res.hasError()) {
            fmt::print(stderr, fg(fmt::color::red), "[PlcRuntime] getJson(DB{}) failed: {}\n", t_db, res.error());
            return {};
        }
        return res.value();
    }

    /// Write a single bit in a DB's raw memory (byte_offset, bit 0-7).
    bool setBit(uint16_t t_db, uint32_t t_byte_offset, int t_bit_index, bool t_value) {
        auto res = impl_->getMemory().writeBit(t_db, t_byte_offset, t_bit_index, t_value);
        if (res.hasError()) {
            fmt::print(stderr, fg(fmt::color::red), "[PlcRuntime] setBit(DB{}.{}.{}) failed: {}\n", t_db, t_byte_offset, t_bit_index,
                res.error().message());
            return false;
        }
        impl_->markDirty(t_db, t_byte_offset, 1);
        return true;
    }

    const runtime::PlcRuntimeSPtr& getImpl() const {
        return impl_;
    }

    ScriptDataBlock* db(uint16_t t_db_num) {
        if (!loopback_conn_) {
            // Create a loopback connection that shares this runtime.
            // 127.0.0.1 is a dummy IP since we don't actually connect to a real PLC,
            // we just use the local in-memory representation.
            loopback_conn_ = std::make_unique<ScriptS7Connection>("127.0.0.1", 0, 0, 102, impl_);
        }
        return new ScriptDataBlock(loopback_conn_.get(), t_db_num);
    }

    // ── Schema introspection — tabular layout views ─────────────────────────

    /// Returns a compact S7 type label (mirrors s7TypeToAS but human-readable).
    static const char* s7TypeLabel(scl::DataType t_type) {
        using T = scl::DataType;
        switch (t_type) {
            case T::Bool:
                return "BOOL";
            case T::Byte:
                return "BYTE";
            case T::Word:
                return "WORD";
            case T::DWord:
                return "DWORD";
            case T::LWord:
                return "LWORD";
            case T::SInt:
                return "SINT";
            case T::USInt:
                return "USINT";
            case T::Int:
                return "INT";
            case T::UInt:
                return "UINT";
            case T::DInt:
                return "DINT";
            case T::UDInt:
                return "UDINT";
            case T::LInt:
                return "LINT";
            case T::ULInt:
                return "ULINT";
            case T::Real:
                return "REAL";
            case T::LReal:
                return "LREAL";
            case T::Time:
                return "TIME";
            case T::LTime:
                return "LTIME";
            case T::Date:
                return "DATE";
            case T::TimeOfDay:
                return "TOD";
            case T::DTL:
            case T::DateTime:
                return "DTL";
            case T::String:
                return "STRING";
            case T::WString:
                return "WSTRING";
            case T::Char:
                return "CHAR";
            case T::WChar:
                return "WCHAR";
            case T::Struct:
                return "STRUCT";
            case T::Counter:
                return "COUNTER";
            case T::Timer:
                return "TIMER";
            default:
                return "?";
        }
    }

    /// Recursively print fields into a table buffer.
    static void printFields(const std::vector<scl::DbField>& t_fields, int t_depth,
        std::vector<std::tuple<std::string, std::string, int, std::string>>& t_rows) {
        const std::string indent(static_cast<size_t>(t_depth) * 2, ' ');
        for (const auto& f : t_fields) {
            std::string type_col;
            if (!f.udt_name.empty()) {
                type_col = f.udt_name;
            } else {
                type_col = s7TypeLabel(f.type);
                if (f.count > 1)
                    type_col += fmt::format("[{}]", f.count);
            }
            int span = s7codec::typeSpanBytes(f.type, f.count > 0 ? f.count : 1);
            if (f.struct_size > 0)
                span = f.struct_size;
            t_rows.emplace_back(indent + f.name, type_col, f.offset, fmt::format("{} B", span));
            // Recurse into children
            if (!f.children.empty())
                printFields(f.children, t_depth + 1, t_rows);
        }
    }

    static void printFieldTable(const std::string& t_header, const std::vector<scl::DbField>& t_fields, int t_total_bytes) {

        // Build rows: {name, type, offset, size}
        std::vector<std::tuple<std::string, std::string, int, std::string>> t_rows;
        printFields(t_fields, 0, t_rows);

        // Compute column widths
        size_t w_name = 20, w_type = 10, w_off = 6, w_size = 8;
        for (auto& [n, t_t, o, t_s] : t_rows) {
            w_name = std::max(w_name, n.size() + 2);
            w_type = std::max(w_type, t_t.size() + 2);
            w_size = std::max(w_size, t_s.size() + 2);
        }
        const size_t total_w = w_name + w_type + w_off + w_size + 5;
        const std::string hline(total_w, '-');

        // Header
        fmt::print("\n");
        fmt::print(fg(fmt::color::steel_blue) | fmt::emphasis::bold, "  {}", t_header);
        fmt::print(" ({} bytes)\n", t_total_bytes);
        fmt::print(fg(fmt::color::dim_gray), "  {}\n", hline);
        fmt::print(fg(fmt::color::dark_gray) | fmt::emphasis::bold, "  {:<{}} {:<{}} {:>{}} {:<{}}\n", "Field", w_name, "Type", w_type,
            "Off", w_off, "Size", w_size);
        fmt::print(fg(fmt::color::dim_gray), "  {}\n", hline);

        // Rows — print color and padding separately so ANSI codes don't skew widths
        for (auto& [n, t_t, o, t_s] : t_rows) {
            bool is_nested = n.starts_with("  ");
            // Name column
            if (is_nested)
                fmt::print(fg(fmt::color::light_gray), "  {}", n);
            else
                fmt::print(fg(fmt::color::white) | fmt::emphasis::bold, "  {}", n);
            // Pad to w_name
            if (n.size() + 2 < w_name)
                fmt::print("{}", std::string(w_name - n.size() - 2, ' '));
            fmt::print(" ");
            // Type column
            fmt::print(fg(fmt::color::medium_aquamarine), "{}", t_t);
            if (t_t.size() < w_type)
                fmt::print("{}", std::string(w_type - t_t.size(), ' '));
            fmt::print(" ");
            // Offset column (right-aligned)
            auto off_str = fmt::format("{}", o);
            if (off_str.size() < w_off)
                fmt::print("{}", std::string(w_off - off_str.size(), ' '));
            fmt::print(fg(fmt::color::gold), "{}", off_str);
            fmt::print(" ");
            // Size column
            fmt::print(fg(fmt::color::light_sea_green), "{}\n", t_s);
        }
        fmt::print(fg(fmt::color::dim_gray), "  {}\n", hline);
    }

    void DBS() {
        const auto& schema = impl_->getSchema();
        const auto& dbs = schema.dbs();
        if (dbs.empty()) {
            fmt::print(fg(fmt::color::yellow), "  [PlcRuntime] No Data Blocks registered.\n");
            return;
        }
        fmt::print(fg(fmt::color::steel_blue) | fmt::emphasis::bold, "\n  ╔══ Data Blocks ({} registered) ══╗\n\n", dbs.size());
        for (const auto& [t_num, t_db] : dbs) {
            const std::string title =
                t_db.db_name.empty() ? fmt::format("DB{}  (unnamed)", t_num) : fmt::format("DB{}  {}", t_num, t_db.db_name);
            printFieldTable(title, t_db.fields, t_db.size_bytes);
        }
        fmt::print("\n");
    }

    void UDTS() {
        const auto& schema = impl_->getSchema();
        const auto& udts = schema.udts();
        if (udts.empty()) {
            fmt::print(fg(fmt::color::yellow), "  [PlcRuntime] No UDT definitions registered.\n");
            return;
        }
        fmt::print(fg(fmt::color::steel_blue) | fmt::emphasis::bold, "\n  ╔══ UDT Definitions ({} registered) ══╗\n\n", udts.size());
        for (const auto& udt : udts) {
            const std::string title = fmt::format("UDT{}  {}  ", udt.udt_number, udt.name);
            printFieldTable(title, udt.fields, udt.size_bytes);
        }
        fmt::print("\n");
    }

private:
    runtime::PlcRuntimeSPtr impl_;
    std::unique_ptr<ScriptS7Connection> loopback_conn_;
    int ref_count_{1};
};

} // namespace sgrn::s7shell::shell
