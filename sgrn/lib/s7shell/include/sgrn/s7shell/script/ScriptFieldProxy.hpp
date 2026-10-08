#pragma once

#include <sgrn/s7shell/script/AngelScriptObject.hpp>
#include <cstdint>
#include <string>

class CScriptArray;
class CScriptDictionary;

namespace sgrn::s7shell::shell
{

class ScriptDataBlock;
struct ScriptDtl;

class ScriptFieldProxy : public AngelScriptObject {
public:
    ScriptFieldProxy(ScriptDataBlock* tp_db, const std::string& t_path);
    ~ScriptFieldProxy() override;

    ScriptDataBlock* getDb() const {
        return db_;
    }
    const std::string& getPath() const {
        return path_;
    }

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
    /// Structured assignment: `proxy = {1.0, 2.0}` / `proxy = {{"a", 1}}`.
    /// Arrays encode element-wise (static arrays need exactly count
    /// elements); struct dicts merge over existing bytes. Nested
    /// arrays/dicts recurse.
    ScriptFieldProxy& assignArray(CScriptArray* tp_arr);
    ScriptFieldProxy& assignDict(CScriptDictionary* tp_dict);

    // ── Field-level network ops (mirror DataBlock::get/put at path_) ──
    /// Network fetch of this field into shadow; returns the JSON value.
    std::string get();
    /// Flush dirty bytes (whole-DB dirty ranges, like db.put()).
    void put();
    /// Shadow + trip immediate writes of this field.
    void put(const std::string& t_raw_val);
    void put(double t_val);
    void put(int32_t t_val);
    void put(bool t_val);
    void putDtl(ScriptDtl* tp_dtl_obj);

    // ── Error introspection (passthrough to the parent DB) ──────────
    bool lastOpOk() const;
    std::string lastOpError() const;

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
    ScriptDataBlock* db_;
    std::string path_;
};

} // namespace sgrn::s7shell::shell
