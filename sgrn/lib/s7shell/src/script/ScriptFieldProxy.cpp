#include <sgrn/s7shell/connection/S7Connection.hpp>
#include <sgrn/s7shell/script/ScriptDataBlock.hpp>
#include <sgrn/s7shell/script/ScriptFieldProxy.hpp>

#include <fmt/format.h>

using ::sgrn::scl::DataType;

namespace sgrn::s7shell::shell
{

ScriptFieldProxy::ScriptFieldProxy(ScriptDataBlock* tp_db, const std::string& t_path)
    : db_(tp_db)
    , path_(t_path) {
    db_->addRef(); // prevent DB from being destroyed while proxy is alive
}

void ScriptFieldProxy::addRef() {
    ++ref_count_;
}

void ScriptFieldProxy::release() {
    if (--ref_count_ == 0) {
        db_->release(); // release our hold on the parent DB
        delete this;
    }
}

// ── Typed writes via s7codec::DecodedValue (no JSON) ──────────────────────

ScriptFieldProxy& ScriptFieldProxy::assignFloat(float t_val) {
    db_->writeDouble(path_, static_cast<double>(t_val));
    return *this;
}

ScriptFieldProxy& ScriptFieldProxy::assignDouble(double t_val) {
    db_->writeDouble(path_, t_val);
    return *this;
}

ScriptFieldProxy& ScriptFieldProxy::assignInt(int32_t t_val) {
    db_->writeInt(path_, t_val);
    return *this;
}

ScriptFieldProxy& ScriptFieldProxy::assignUInt(uint32_t t_val) {
    db_->writeScalar(path_, s7codec::DecodedValue::makeUnsigned(t_val));
    return *this;
}

ScriptFieldProxy& ScriptFieldProxy::assignInt8(int8_t t_val) {
    db_->writeScalar(path_, s7codec::DecodedValue::makeSigned(t_val));
    return *this;
}

ScriptFieldProxy& ScriptFieldProxy::assignUInt8(uint8_t t_val) {
    db_->writeScalar(path_, s7codec::DecodedValue::makeUnsigned(t_val));
    return *this;
}

ScriptFieldProxy& ScriptFieldProxy::assignInt16(int16_t t_val) {
    db_->writeScalar(path_, s7codec::DecodedValue::makeSigned(t_val));
    return *this;
}

ScriptFieldProxy& ScriptFieldProxy::assignUInt16(uint16_t t_val) {
    db_->writeScalar(path_, s7codec::DecodedValue::makeUnsigned(t_val));
    return *this;
}

ScriptFieldProxy& ScriptFieldProxy::assignInt64(int64_t t_val) {
    db_->writeScalar(path_, s7codec::DecodedValue::makeSigned(t_val));
    return *this;
}

ScriptFieldProxy& ScriptFieldProxy::assignUInt64(uint64_t t_val) {
    db_->writeScalar(path_, s7codec::DecodedValue::makeUnsigned(t_val));
    return *this;
}

ScriptFieldProxy& ScriptFieldProxy::assignBool(bool t_val) {
    db_->writeBool(path_, t_val);
    return *this;
}

ScriptFieldProxy& ScriptFieldProxy::assignString(const std::string& t_val) {
    if (db_ && db_->conn_) {
        if (auto loc = db_->conn_->schema_.findField(db_->db_num_, path_)) {
            if (loc->field->type == DataType::Char || loc->field->type == DataType::WChar) {
                uint8_t char_val = t_val.empty() ? 0 : static_cast<uint8_t>(t_val[0]);
                db_->writeScalar(path_, s7codec::DecodedValue::makeUnsigned(char_val));
                return *this;
            }
        }
    }
    db_->writeScalar(path_, s7codec::DecodedValue::makeString(t_val));
    return *this;
}

ScriptFieldProxy& ScriptFieldProxy::assignDtl(ScriptDtl* tp_dtl_obj) {
    db_->writeDtl(path_, tp_dtl_obj);
    return *this;
}

// ── Typed reads ───────────────────────────────────────────────────────────

float ScriptFieldProxy::toFloat() const {
    return static_cast<float>(db_->getReal(path_));
}

double ScriptFieldProxy::toDouble() const {
    return db_->getReal(path_);
}

int32_t ScriptFieldProxy::toInt() const {
    auto dv = db_->readScalar(path_);
    if (dv.kind() == s7codec::ValueKind::SignedInt)
        return static_cast<int32_t>(dv.i());
    if (dv.kind() == s7codec::ValueKind::UnsignedInt)
        return static_cast<int32_t>(dv.u());
    return 0;
}

uint32_t ScriptFieldProxy::toUInt() const {
    auto dv = db_->readScalar(path_);
    if (dv.kind() == s7codec::ValueKind::UnsignedInt)
        return static_cast<uint32_t>(dv.u());
    if (dv.kind() == s7codec::ValueKind::SignedInt)
        return static_cast<uint32_t>(dv.i());
    return 0;
}

int8_t ScriptFieldProxy::toInt8() const {
    return static_cast<int8_t>(toInt());
}

uint8_t ScriptFieldProxy::toUInt8() const {
    return static_cast<uint8_t>(toUInt());
}

int16_t ScriptFieldProxy::toInt16() const {
    return static_cast<int16_t>(toInt());
}

uint16_t ScriptFieldProxy::toUInt16() const {
    return static_cast<uint16_t>(toUInt());
}

int64_t ScriptFieldProxy::toInt64() const {
    auto dv = db_->readScalar(path_);
    if (dv.kind() == s7codec::ValueKind::SignedInt)
        return dv.i();
    if (dv.kind() == s7codec::ValueKind::UnsignedInt)
        return static_cast<int64_t>(dv.u());
    return 0;
}

uint64_t ScriptFieldProxy::toUInt64() const {
    auto dv = db_->readScalar(path_);
    if (dv.kind() == s7codec::ValueKind::UnsignedInt)
        return dv.u();
    if (dv.kind() == s7codec::ValueKind::SignedInt)
        return static_cast<uint64_t>(dv.i());
    return 0;
}

bool ScriptFieldProxy::toBool() const {
    return db_->getBool(path_);
}

std::string ScriptFieldProxy::toString() const {
    return db_->val(path_);
}

void ScriptFieldProxy::print() const {
    fmt::print("{}\n", db_->val(path_));
}

// ── Chaining ──────────────────────────────────────────────────────────────

ScriptFieldProxy* ScriptFieldProxy::index(const std::string& t_key) {
    return new ScriptFieldProxy(db_, path_ + "." + t_key);
}

ScriptFieldProxy* ScriptFieldProxy::indexInt(int t_idx) {
    return new ScriptFieldProxy(db_, path_ + "[" + std::to_string(t_idx) + "]");
}

} // namespace sgrn::s7shell::shell
