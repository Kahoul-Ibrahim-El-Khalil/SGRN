#pragma once

#include <cstdint>
#include <string>

namespace sgrn::s7shell::shell
{

class ScriptDataBlock;
struct ScriptDtl;

class ScriptFieldProxy {
public:
    ScriptFieldProxy(ScriptDataBlock* tp_db, const std::string& t_path);
    ~ScriptFieldProxy() = default;

    ScriptDataBlock* getDb() const {
        return db_;
    }
    const std::string& getPath() const {
        return path_;
    }

    void addRef();
    void release();

    // ── Typed assignment (AngelScript opAssign overloads) ─────────────
    ScriptFieldProxy& assignFloat(float t_val);
    ScriptFieldProxy& assignDouble(double t_val);
    ScriptFieldProxy& assignInt(int32_t t_val);
    ScriptFieldProxy& assignUInt(uint32_t t_val);
    ScriptFieldProxy& assignInt8(int8_t t_val);
    ScriptFieldProxy& assignUInt8(uint8_t t_val);
    ScriptFieldProxy& assignInt16(int16_t t_val);
    ScriptFieldProxy& assignUInt16(uint16_t t_val);
    ScriptFieldProxy& assignInt64(int64_t t_val);
    ScriptFieldProxy& assignUInt64(uint64_t t_val);
    ScriptFieldProxy& assignBool(bool t_val);
    ScriptFieldProxy& assignString(const std::string& t_val);
    ScriptFieldProxy& assignDtl(ScriptDtl* tp_dtl_obj);

    // ── Typed reads ──────────────────────────────────────────────────
    float toFloat() const;
    double toDouble() const;
    int32_t toInt() const;
    uint32_t toUInt() const;
    int8_t toInt8() const;
    uint8_t toUInt8() const;
    int16_t toInt16() const;
    uint16_t toUInt16() const;
    int64_t toInt64() const;
    uint64_t toUInt64() const;
    bool toBool() const;
    std::string toString() const;

    // ── Inspection ───────────────────────────────────────────────────
    void print() const; // print JSON value to stdout

    // ── Chaining: proxy["substruct"]["field"] ────────────────────────
    ScriptFieldProxy* index(const std::string& t_key);
    ScriptFieldProxy* indexInt(int t_idx);

private:
    int ref_count_{1};
    ScriptDataBlock* db_;
    std::string path_;
};

} // namespace sgrn::s7shell::shell
