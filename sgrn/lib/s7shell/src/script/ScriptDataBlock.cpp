#include <sgrn/gateway/twin/twin.hpp>
#include <sgrn/plcsim/runtime/PlcRuntime.hpp>
#include <sgrn/s7shell/connection/S7Connection.hpp>
#include <sgrn/s7shell/script/ScriptDataBlock.hpp>
#include <sgrn/s7shell/script/ScriptFieldProxy.hpp>
#include <sgrn/s7shell/script/ScriptPathBatch.hpp>
#include <sgrn/s7shell/script/ScriptTagTable.hpp>
#include <sgrn/s7shell/utils/json_helpers.hpp>
#include <sgrn/scl/types.hpp>

#include <sgrn/s7shell/errors.hpp>

#include <fmt/color.h>
#include <fmt/format.h>
#include <algorithm>
#include <angelscript.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>
#include <string>
#include <thread>

namespace sgrn::s7shell::shell
{

using namespace sgrn::scl;
using namespace ::sgrn::gateway::twin;

static void markDirtyDiff(const ::sgrn::plcsim::runtime::PlcRuntimeSPtr& tsp_runtime, uint16_t t_db_num, size_t t_base_offset,
    const uint8_t* tp_before, const uint8_t* tp_after, size_t t_len) {
    if (!tsp_runtime || t_len == 0)
        return;

    size_t start = 0;
    bool in_dirty_region = false;
    for (size_t i = 0; i < t_len; ++i) {
        if (tp_before[i] != tp_after[i]) {
            if (!in_dirty_region) {
                start = i;
                in_dirty_region = true;
            }
        } else if (in_dirty_region) {
            tsp_runtime->markDirty(t_db_num, static_cast<uint32_t>(t_base_offset + start), static_cast<uint32_t>(i - start));
            in_dirty_region = false;
        }
    }
    if (in_dirty_region)
        tsp_runtime->markDirty(t_db_num, static_cast<uint32_t>(t_base_offset + start), static_cast<uint32_t>(t_len - start));
}

ScriptDataBlock::ScriptDataBlock(ScriptS7Connection* tp_conn, uint16_t t_db_num)
    : conn_(tp_conn)
    , db_num_(t_db_num) {
    auto db_res = conn_->schema_.getDb(t_db_num);
    if (!db_res.hasError()) {
        db_size_ = db_res.value()->size_bytes;
        auto it = conn_->db_snapshots_.find(t_db_num);
        if (it != conn_->db_snapshots_.end()) {
            snapshot_buffer_ = it->second;
        } else {
            snapshot_buffer_.assign(db_size_, 0); // zero-init so first push() detects all bytes as dirty
            conn_->db_snapshots_[db_num_] = snapshot_buffer_;
        }
    }
}

ScriptDataBlock::~ScriptDataBlock() = default;

void ScriptDataBlock::notifyConnError(const ::sgrn::scl::SclError& t_err) {
    if (conn_) {
        conn_->setLastError(t_err);
    }
}

void ScriptDataBlock::notifyConnError(const ::sgrn::wrappers::s7::S7Error& t_err) {
    if (conn_) {
        conn_->setLastError(t_err);
    }
}

void ScriptDataBlock::addRef() {
    ++ref_count_;
}

void ScriptDataBlock::release() {
    if (--ref_count_ == 0)
        delete this;
}

void ScriptDataBlock::registerSize(size_t t_size) {
    db_size_ = t_size;
    if (snapshot_buffer_.size() < t_size)
        snapshot_buffer_.resize(t_size, 0);
}

void ScriptDataBlock::addField(const std::string& t_name, const std::string& t_type_str, uint32_t t_offset, uint16_t t_count) {
    scl::DbSchema db;
    auto res = conn_->schema_.getDb(db_num_);
    if (!res.hasError()) {
        db = *res.value();
    } else {
        db.db_number = db_num_;
        db.size_bytes = db_size_;
        db.db_name = fmt::format("DB{}", db_num_);
    }

    scl::DbField field;
    field.name = t_name;
    field.offset = static_cast<int>(t_offset);
    field.count = static_cast<int>(t_count);

    if (auto t = scl::parseS7Type(t_type_str)) {
        field.type = *t;
    } else if (conn_->schema_.hasUdt(t_type_str)) {
        field.udt_name = t_type_str;
        if (auto sub = conn_->schema_.getUdtByName(t_type_str); !sub.hasError()) {
            field.children = sub.value()->fields;
            field.struct_size = sub.value()->size_bytes;
        }
    } else {
        field.type = DataType::Byte;
    }

    db.fields.push_back(std::move(field));
    if (!shell::ok(conn_->schema_.addDb(std::move(db), true), "DataBlock::addField"))
        return;
    (void)conn_->memory_.loadRegistry(conn_->schema_);
}

ScriptDataBlock* ScriptDataBlock::get() {
    if (db_size_ == 0) {
        // Bug fix: cannot perform a read without knowing the DB size.
        // Silently returning here produced zero/stale data with no indication of failure.
        fmt::print(stderr,
            "[DataBlock] get() called on DB{} but db_size_ is 0. "
            "Call get(size_t) or ensure a schema is loaded.\n",
            db_num_);
        last_op_ok_ = false;
        last_op_err_ = DbIoError::LocalMemoryFailed;
        addRef();
        return this;
    }
    return get(db_size_); // get(size_t) handles addRef
}

ScriptDataBlock* ScriptDataBlock::get(size_t t_total_size) {
    db_size_ = t_total_size;
    snapshot_buffer_.assign(t_total_size, 0);

    if (!conn_->client_.isConnected()) {
        // Offline mode: populate snapshot from local memory arena if available,
        // so that getters read back what setters wrote without needing a PLC.
        if (auto r = conn_->memory_.readDbMemory(db_num_, 0, t_total_size, snapshot_buffer_.data()); r) {
            snapshot_valid_ = true;
            conn_->db_snapshots_[db_num_] = snapshot_buffer_;
            last_op_ok_ = true;
        } else {
            // No data in local arena — client is not connected and no prior read.
            // Mark as failed so scripts can detect this situation.
            fmt::print(stderr,
                "[DataBlock] get() on DB{}: not connected and no local data available. "
                "Values will be zero. Check connection with isConnected().\n",
                db_num_);
            last_op_ok_ = false;
            last_op_err_ = DbIoError::NetworkReadFailed;
        }
        addRef();
        return this;
    }

    // Dynamically determine chunk size based on negotiated PDU length
    int pdu = shell::valueOr(conn_->client_.getNegotiatedPduLength(), 240);
    size_t chunk_size = std::max(size_t(64), size_t(pdu - 32));

    for (size_t t_offset = 0; t_offset < t_total_size; t_offset += chunk_size) {
        size_t current_chunk = std::min(chunk_size, t_total_size - t_offset);

        auto res = conn_->client_.readArea(S7AreaDB, db_num_, static_cast<int>(t_offset), static_cast<int>(current_chunk), S7WLByte);
        if (res.hasError()) {
            shell::logError(res.error(), "DataBlock::get");
            (void)setOpResult(res);
            // Must addRef before returning — AS will Release this handle
            addRef();
            return this;
        }

        std::memcpy(snapshot_buffer_.data() + t_offset, res.value().data(), current_chunk);
    }

    if (auto r = conn_->memory_.writeDbMemory(db_num_, 0, t_total_size, snapshot_buffer_.data()); !r) {
        fmt::print(stderr, "DataBlock::fetch: failed to write to local memory for DB{}: {}\n", db_num_, r.error());
        // Don't return nullptr — AngelScript will segfault on a null handle. Return self with error set.
        last_op_ok_ = false;
        last_op_err_ = DbIoError::NetworkWriteFailed;
        addRef();
        return this;
    }

    // If a PlcRuntime is attached, compare the newly read snapshot against the previous
    // baseline and fire markDirtyDiff for any changed byte regions.
    // This makes db.get() drive the PersistenceBridge in live-polling scripts.
    if (conn_->runtime_) {
        auto it = conn_->db_snapshots_.find(db_num_);
        if (it != conn_->db_snapshots_.end() && it->second.size() == t_total_size) {
            markDirtyDiff(conn_->runtime_, db_num_, 0, it->second.data(), snapshot_buffer_.data(), t_total_size);
        }
    }

    snapshot_valid_ = true;
    // Persist as shared baseline: future db() instances for this DB number
    // start from confirmed PLC data, not a stale or zero-init buffer.
    conn_->db_snapshots_[db_num_] = snapshot_buffer_;
    last_op_ok_ = true;
    addRef();
    return this;
}

void ScriptDataBlock::push() {
    if (db_size_ == 0) {
        throwScriptException("No schema loaded and DB size not registered", ShellError::Generic);
        return;
    }

    // Flush any pending write commands to the local memory arena
    conn_->memory_.processor()->processCommands();

    std::vector<uint8_t> current_mem(db_size_);
    if (!(conn_->memory_.readDbMemory(db_num_, 0, db_size_, current_mem.data()))) {
        throwScriptException(fmt::format("DataBlock::push: failed to read local memory for DB{}", db_num_), ShellError::Generic);
        return;
    }

    // If not connected to a real PLC, update the snapshot so getters see the new
    // values (write-through to snapshot = "commit to local memory").
    if (!conn_->client_.isConnected()) {
        snapshot_buffer_ = current_mem;
        conn_->db_snapshots_[db_num_] = current_mem;
        last_op_ok_ = true;
        return;
    }

    // Automatically detect dirty regions by comparing current memory vs snapshot.
    // Abort on first writeArea failure — a timeout mid-push should not silently
    // skip later segments and leave the PLC in a partially-written state.
    size_t start = 0;
    bool in_dirty_region = false;
    bool write_failed = false;

    for (size_t i = 0; i < db_size_ && !write_failed; ++i) {
        if (current_mem[i] != snapshot_buffer_[i]) {
            if (!in_dirty_region) {
                start = i;
                in_dirty_region = true;
            }
        } else {
            if (in_dirty_region) {
                auto res = conn_->client_.writeArea(
                    S7AreaDB, db_num_, static_cast<int>(start), static_cast<int>(i - start), S7WLByte, current_mem.data() + start);
                if (res.hasError()) {
                    (void)setOpResult(res);
                    write_failed = true;
                    auto shell_err = fromS7Error(res.error());
                    throwScriptException(fmt::format("DB{}.push (segment @{}, len {}) failed", db_num_, start, i - start),
                        shell_err == ShellError::Generic ? ShellError::WriteFailed : shell_err);
                }
                in_dirty_region = false;
            }
        }
    }
    if (in_dirty_region && !write_failed) {
        auto res = conn_->client_.writeArea(
            S7AreaDB, db_num_, static_cast<int>(start), static_cast<int>(db_size_ - start), S7WLByte, current_mem.data() + start);
        if (res.hasError()) {
            (void)setOpResult(res);
            write_failed = true;
            auto shell_err = fromS7Error(res.error());
            throwScriptException(fmt::format("DB{}.push (segment @{}, len {}) failed", db_num_, start, db_size_ - start),
                shell_err == ShellError::Generic ? ShellError::WriteFailed : shell_err);
        }
    }
    if (write_failed) {
        // Exception already thrown in the loop.
        return;
    }

    // Update snapshot to current state
    snapshot_buffer_ = current_mem;
    conn_->db_snapshots_[db_num_] = current_mem;
    last_op_ok_ = true;
}

void ScriptDataBlock::writeScalar(const std::string& t_path, const s7codec::DecodedValue& t_val) {
    if (!conn_->runtime_)
        return;
    auto loc = conn_->runtime_->getSchema().findField(db_num_, t_path);
    if (!loc) {
        throwScriptException(fmt::format("Field not found: {}", t_path), ShellError::NotFound);
        return;
    }

    int sz = (loc->field->type == DataType::Bool && loc->field->count <= 1) ? 1 : ::sgrn::gateway::twin::fieldSpanSize(*loc->field);
    std::vector<uint8_t> buf(sz, 0);

    // Read existing bytes first to preserve other bits (especially for bit-packed BOOL arrays)
    readFieldFromMemory(loc->abs_offset, buf.data(), sz);

    auto status =
        s7codec::encodeScalar(t_val, loc->field->type, buf.data(), sz, loc->field->bit_index, loc->field->count, loc->field->endianness);
    if (!status.has_value()) {
        throwScriptException(s7codec::toString(status.error()), ShellError::TypeMismatch);
        return;
    }

    // We can directly call writeFieldToMemory which handles the write and marks it dirty
    writeFieldToMemory(loc->abs_offset, buf.data(), sz);
}

void ScriptDataBlock::writeDouble(const std::string& t_path, double t_val) {
    writeScalar(t_path, s7codec::DecodedValue::makeDouble(t_val));
}

void ScriptDataBlock::writeInt(const std::string& t_path, int32_t t_val) {
    writeScalar(t_path, s7codec::DecodedValue::makeSigned(t_val));
}

void ScriptDataBlock::writeBool(const std::string& t_path, bool t_val) {
    writeScalar(t_path, s7codec::DecodedValue::makeBool(t_val));
}

/// Short field descriptor for assignment error messages, e.g.
/// "ARRAY[4] OF REAL", "STRUCT \"Motor\"", "REAL".
static std::string describeField(const ::sgrn::scl::DbField& t_field) {
    const char* tp_type = "???";
    switch (t_field.type) {
        case DataType::Bool:
            tp_type = "BOOL";
            break;
        case DataType::Byte:
            tp_type = "BYTE";
            break;
        case DataType::Word:
            tp_type = "WORD";
            break;
        case DataType::DWord:
            tp_type = "DWORD";
            break;
        case DataType::SInt:
            tp_type = "SINT";
            break;
        case DataType::USInt:
            tp_type = "USINT";
            break;
        case DataType::Int:
            tp_type = "INT";
            break;
        case DataType::UInt:
            tp_type = "UINT";
            break;
        case DataType::DInt:
            tp_type = "DINT";
            break;
        case DataType::UDInt:
            tp_type = "UDINT";
            break;
        case DataType::LInt:
            tp_type = "LINT";
            break;
        case DataType::ULInt:
            tp_type = "ULINT";
            break;
        case DataType::LWord:
            tp_type = "LWORD";
            break;
        case DataType::Real:
            tp_type = "REAL";
            break;
        case DataType::LReal:
            tp_type = "LREAL";
            break;
        case DataType::Time:
            tp_type = "TIME";
            break;
        case DataType::LTime:
            tp_type = "LTIME";
            break;
        case DataType::Date:
            tp_type = "DATE";
            break;
        case DataType::TimeOfDay:
            tp_type = "TOD";
            break;
        case DataType::LTimeOfDay:
            tp_type = "LTOD";
            break;
        case DataType::DateTime:
            tp_type = "DT";
            break;
        case DataType::LDT:
            tp_type = "LDT";
            break;
        case DataType::LDTL:
            tp_type = "LDTL";
            break;
        case DataType::DTL:
            tp_type = "DTL";
            break;
        case DataType::Char:
            tp_type = "CHAR";
            break;
        case DataType::WChar:
            tp_type = "WCHAR";
            break;
        case DataType::String:
            tp_type = "STRING";
            break;
        case DataType::WString:
            tp_type = "WSTRING";
            break;
        case DataType::XString:
            tp_type = "XSTRING";
            break;
        case DataType::XWString:
            tp_type = "XWSTRING";
            break;
        case DataType::Counter:
            tp_type = "COUNTER";
            break;
        case DataType::Timer:
            tp_type = "TIMER";
            break;
        case DataType::Struct:
            tp_type = "STRUCT";
            break;
    }
    if (t_field.type == DataType::Struct && !t_field.udt_name.empty())
        return t_field.count > 1 ? fmt::format("ARRAY[{}] OF \"{}\"", t_field.count, t_field.udt_name)
                                 : fmt::format("STRUCT \"{}\"", t_field.udt_name);
    if (t_field.count > 1)
        return fmt::format("ARRAY[{}] OF {}", t_field.count, tp_type);
    return std::string(tp_type);
}

void ScriptDataBlock::writeJson(const std::string& t_path, const std::string& t_json_val) {
    ::sgrn::scl::DbField target_field{};
    size_t abs_offset = 0;
    bool found = false;
    // Runtime schema is authoritative for staging (same source as
    // writeScalar); fall back to the connection schema for runtime-less
    // file mode.
    if (conn_->runtime_) {
        if (auto loc = conn_->runtime_->getSchema().findField(db_num_, t_path)) {
            target_field = *loc->field;
            abs_offset = loc->abs_offset;
            found = true;
        }
    } else if (auto loc = conn_->schema_.findField(db_num_, t_path)) {
        target_field = *loc->field;
        abs_offset = loc->abs_offset;
        found = true;
    }
    if (!found) {
        throwScriptException(fmt::format("DB{}: field '{}' not found in schema", db_num_, t_path), ShellError::NotFound);
        return;
    }
    if (!t_path.empty() && t_path.back() == ']') {
        target_field.count = 1;
    }
    // Shape pre-checks with actionable errors (the encoder itself only
    // reports a bare Generic on mismatch).
    {
        rapidjson::Document doc;
        if (!doc.Parse(t_json_val.c_str()).HasParseError()) {
            const bool is_struct = target_field.type == DataType::Struct && target_field.count <= 1;
            const bool is_array = target_field.count > 1 && !target_field.is_dynamic;
            if (doc.IsObject() && !is_struct) {
                throwScriptException(
                    fmt::format("DB{}.'{}' is {} — objects assign only to STRUCT fields", db_num_, t_path, describeField(target_field)),
                    ShellError::TypeMismatch);
                return;
            }
            if (doc.IsArray() && !is_array && !target_field.is_dynamic) {
                throwScriptException(
                    fmt::format("DB{}.'{}' is {} — arrays assign only to ARRAY fields", db_num_, t_path, describeField(target_field)),
                    ShellError::TypeMismatch);
                return;
            }
            // Static arrays are fixed-size PLC memory: require exactly count
            // elements (same rule as put(path, json)). Dynamic arrays accept
            // any length up to capacity; struct dicts merge over existing
            // bytes.
            if (is_array && doc.Size() != static_cast<rapidjson::SizeType>(target_field.count)) {
                throwScriptException(
                    fmt::format("DB{}.'{}' is {} and needs exactly {} elements, got {} — assign a full-length value (zeros to clear)",
                        db_num_, t_path, describeField(target_field), target_field.count, doc.Size()),
                    ShellError::TypeMismatch);
                return;
            }
        }
    }
    const int span = ::sgrn::gateway::twin::fieldSpanSize(target_field);
    if (span <= 0) {
        throwScriptException(
            fmt::format("DB{}.'{}' has zero span ({})", db_num_, t_path, describeField(target_field)), ShellError::Generic);
        return;
    }
    // Read existing bytes first so partial struct dicts merge and boolean
    // bits in shared bytes are preserved.
    std::vector<uint8_t> tp_before(static_cast<size_t>(span), 0);
    std::vector<uint8_t> buf(static_cast<size_t>(span), 0);
    if (!(conn_->memory_.readDbMemory(db_num_, abs_offset, tp_before.size(), tp_before.data()))) {
        throwScriptException("readDbMemory failed", ShellError::Generic);
        return;
    }
    buf = tp_before;
    auto res =
        ::sgrn::gateway::twin::encodeFieldAt(target_field, t_json_val, buf.data(), static_cast<size_t>(span), 0, target_field.endianness);
    if (res.hasError()) {
        throwScriptException(fmt::format("DB{}.'{}' rejected the value for {}: {}", db_num_, t_path, describeField(target_field),
                                 ::sgrn::scl::toString(res.error())),
            ShellError::TypeMismatch);
        return;
    }
    if (auto r = conn_->memory_.writeDbMemory(db_num_, abs_offset, buf.size(), buf.data()); !r) {
        throwScriptException(fmt::format("writeDbMemory failed: {}", r.error()), ShellError::Generic);
        return;
    }
    snapshot_valid_ = true;
    markDirtyDiff(conn_->runtime_, db_num_, abs_offset, tp_before.data(), buf.data(), buf.size());
}

std::string ScriptDataBlock::val(const std::string& t_path) {
    conn_->memory_.processor()->processCommands();
    if (!snapshot_valid_) {
        auto refresh = conn_->getOrCreateDbProvider(db_num_)->get(conn_->client_, t_path);
        if (refresh.hasError()) {
            throwScriptException(fmt::format("DB{}.val('{}') [S7 read] failed", db_num_, t_path), fromDbIoError(refresh.error()));
            return "null";
        }
        snapshot_valid_ = true;
    }
    auto res = conn_->getOrCreateDbProvider(db_num_)->read(t_path);
    if (res.hasError()) {
        throwScriptException(fmt::format("DB{}.val('{}') [decode] failed", db_num_, t_path), fromDbIoError(res.error()));
        return "null";
    }
    return res.value();
}

std::string ScriptDataBlock::get(const std::string& t_path) {
    conn_->memory_.processor()->processCommands();
    auto* p_provider = conn_->getOrCreateDbProvider(db_num_);
    if (!p_provider) {
        last_op_ok_ = false;
        last_op_err_ = DbIoError::LocalMemoryFailed;
        throwScriptException(fmt::format("DB{}.get('{}') failed: no provider", db_num_, t_path), ShellError::Generic);
        return "null";
    }
    if (!conn_->client_.isConnected()) {
        // Offline: serve the twin shadow (same source the online path decodes
        // after committing the wire read) instead of throwing NotConnected —
        // mirrors tags.get() on runtime tags. A dropped link (vs virtual use
        // that never connected) still flags NotConnected: the value is
        // best-effort shadow, and the failed trip must stay visible.
        auto res = p_provider->read(t_path);
        if (res.hasError()) {
            last_op_ok_ = false;
            last_op_err_ = res.error();
            throwScriptException(fmt::format("DB{}.get('{}') failed", db_num_, t_path), fromDbIoError(res.error()));
            return "null";
        }
        snapshot_valid_ = true;
        if (conn_->wasConnected()) {
            last_op_ok_ = false;
            last_op_err_ = DbIoError::NotConnected;
            conn_->setLastError(S7Error::NotConnected);
        } else {
            last_op_ok_ = true;
        }
        return res.value();
    }
    auto res = p_provider->get(conn_->client_, t_path);
    if (res.hasError()) {
        last_op_ok_ = false;
        last_op_err_ = res.error();
        if (conn_)
            conn_->setLastError(S7Error::ReadError);
        throwScriptException(fmt::format("DB{}.get('{}') failed", db_num_, t_path), fromDbIoError(res.error()));
        return "null";
    }
    snapshot_valid_ = true;
    last_op_ok_ = true;
    return res.value();
}

s7codec::DecodedValue ScriptDataBlock::readScalar(const std::string& t_path) {
    if (!conn_->runtime_)
        return {};
    auto loc = conn_->runtime_->getSchema().findField(db_num_, t_path);
    if (!loc) {
        throwScriptException(fmt::format("Field not found: {}", t_path), ShellError::NotFound);
        return {};
    }

    // Ensure the snapshot is fetched if it hasn't been yet (wire refresh when
    // online; twin memory is already current offline, mirroring get()).
    // Degraded-trip flag for dropped links (virtual use stays clean).
    if (!conn_->client_.isConnected() && conn_->wasConnected())
        conn_->setLastError(S7Error::NotConnected);
    conn_->memory_.processor()->processCommands();
    if (!snapshot_valid_ && conn_->client_.isConnected()) {
        auto refresh = conn_->getOrCreateDbProvider(db_num_)->get(conn_->client_, t_path);
        if (refresh.hasError()) {
            throwScriptException(fmt::format("DB{}.readScalar('{}') [S7 read] failed", db_num_, t_path), fromDbIoError(refresh.error()));
            return {};
        }
        snapshot_valid_ = true;
    }

    int sz = (loc->field->type == DataType::Bool && loc->field->count <= 1) ? 1 : ::sgrn::gateway::twin::fieldSpanSize(*loc->field);
    std::vector<uint8_t> tmp(sz, 0);
    readFieldFromMemory(loc->abs_offset, tmp.data(), sz);
    return s7codec::decodeScalar(loc->field->type, tmp.data(), sz, loc->field->bit_index, loc->field->count, loc->field->endianness);
}
double ScriptDataBlock::getReal(const std::string& t_path) {
    auto dv = readScalar(t_path);
    if (dv.kind() == s7codec::ValueKind::Float)
        return dv.f();
    if (dv.kind() == s7codec::ValueKind::Double)
        return dv.d();
    if (dv.kind() == s7codec::ValueKind::SignedInt)
        return static_cast<double>(dv.i());
    if (dv.kind() == s7codec::ValueKind::UnsignedInt)
        return static_cast<double>(dv.u());
    return 0.0;
}

int32_t ScriptDataBlock::getInt(const std::string& t_path) {
    auto dv = readScalar(t_path);
    if (dv.kind() == s7codec::ValueKind::SignedInt)
        return static_cast<int32_t>(dv.i());
    if (dv.kind() == s7codec::ValueKind::UnsignedInt)
        return static_cast<int32_t>(dv.u());
    if (dv.kind() == s7codec::ValueKind::Float)
        return static_cast<int32_t>(dv.f());
    if (dv.kind() == s7codec::ValueKind::Double)
        return static_cast<int32_t>(dv.d());
    return 0;
}

bool ScriptDataBlock::getBool(const std::string& t_path) {
    auto dv = readScalar(t_path);
    if (dv.kind() == s7codec::ValueKind::Bool)
        return dv.b();
    if (dv.kind() == s7codec::ValueKind::SignedInt)
        return dv.i() != 0;
    if (dv.kind() == s7codec::ValueKind::UnsignedInt)
        return dv.u() != 0;
    return false;
}

void ScriptDataBlock::writeDtl(const std::string& t_path, ScriptDtl* tp_dtl_obj) {
    if (!tp_dtl_obj)
        return;
    writeScalar(t_path, s7codec::DecodedValue::makeString(tp_dtl_obj->timestamp_str_));
}

void ScriptDataBlock::commitBaseline(const std::string& t_path, const std::string& t_json_val) {
    // Write-back: encode the confirmed-written field into this instance's
    // snapshot AND the shared dbSnapshots_ baseline. Future db() instances
    // will start from the confirmed PLC state without a get() roundtrip.
    if (auto loc = conn_->schema_.findField(db_num_, t_path)) {
        if (snapshot_buffer_.size() >= db_size_) {
            (void)::sgrn::gateway::twin::encodeFieldAt(
                *loc->field, t_json_val, snapshot_buffer_.data() + loc->abs_offset, db_size_ - loc->abs_offset, 0, loc->field->endianness);
            conn_->db_snapshots_[db_num_] = snapshot_buffer_;
        }
    }
    // Ledger mark so gateway publish / auto-broadcast observe the write.
    // Full-DB granularity (coalesced by the ledger), mirroring set().
    if (conn_->runtime_) {
        if (auto db_res = conn_->schema_.getDb(db_num_); !db_res.hasError() && db_res.value())
            conn_->runtime_->markDirty(db_num_, 0, static_cast<uint32_t>(db_res.value()->size_bytes));
    }
}

void ScriptDataBlock::put(const std::string& t_path, const std::string& t_raw_val) {
    const std::string t_json_val = ::sgrn::gateway::twin::parseRawValuePayload(t_raw_val);
    auto* p_provider = conn_->getOrCreateDbProvider(db_num_);
    if (!p_provider) {
        last_op_ok_ = false;
        last_op_err_ = DbIoError::LocalMemoryFailed;
        fmt::print("DB{}.put('{}', '{}') failed: no provider (unknown DB)", db_num_, t_path, t_raw_val);
        return;
    }
    if (!conn_->client_.isConnected()) {
        // Offline: twin (shadow) write only, no trip — mirrors tagPut.
        // Flush the queued command so the arena (and any reader of it)
        // observes the value synchronously. Dropped links flag NotConnected
        // (failed trip stays visible); virtual use stays clean.
        auto res = p_provider->write(t_path, t_json_val);
        if (!setOpResult(res)) {
            fmt::print("DB{}.put('{}', '{}') failed: Error:: {}", db_num_, t_path, t_raw_val, res.error());
            return;
        }
        conn_->memory_.processor()->processCommands();
        commitBaseline(t_path, t_json_val);
        if (conn_->wasConnected()) {
            last_op_ok_ = false;
            last_op_err_ = DbIoError::NotConnected;
            conn_->setLastError(S7Error::NotConnected);
        } else {
            last_op_ok_ = true;
        }
        return;
    }
    auto res = p_provider->put(conn_->client_, t_path, t_json_val);
    if (!setOpResult(res)) {
        fmt::print("DB{}.put('{}', '{}') failed: Error:: {}", db_num_, t_path, t_raw_val, res.error());
        return;
    }
    commitBaseline(t_path, t_json_val);
}

void ScriptDataBlock::putDouble(const std::string& t_path, double t_val) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.Double(t_val);
    // Explicit std::string: a bare const char* would resolve to the
    // put(string, bool) overload (pointer→bool beats user conversion) and
    // write "true" instead of the number.
    put(t_path, std::string(sb.GetString()));
}

void ScriptDataBlock::putInt(const std::string& t_path, int32_t t_val) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.Int(t_val);
    // See putDouble: explicit std::string defeats the bool overload.
    put(t_path, std::string(sb.GetString()));
}

void ScriptDataBlock::putBool(const std::string& t_path, bool t_val) {
    // Explicit strings: there is no put(string, bool) overload — a literal
    // pair would decay to const char* and misbind.
    put(t_path, t_val ? std::string("true") : std::string("false"));
}

void ScriptDataBlock::putDtl(const std::string& t_path, ScriptDtl* tp_dtl_obj) {
    if (!tp_dtl_obj)
        return;
    // toString() wraps timestamp_str in JSON quotes
    put(t_path, tp_dtl_obj->toString());
}

void ScriptDataBlock::put() {
    push();
}

S7PathBatch* ScriptDataBlock::getPath(const std::string& t_p) {
    auto* p_batch = new S7PathBatch(this);
    p_batch->path(t_p);
    return p_batch;
}

ScriptFieldProxy* ScriptDataBlock::opIndex(const std::string& t_ey) {
    return new ScriptFieldProxy(this, t_ey);
}

std::string ScriptDataBlock::getDbName() const {
    auto res = conn_->schema_.getDb(db_num_);
    if (res.hasError()) {
        return {};
    }
    return res.value()->db_name;
}

std::string ScriptDataBlock::toJson() const {
    if (auto result = conn_->memory_.getSubtreeJson(db_num_, ""); result.hasError()) {

        return std::string{"{}"};
    } else {
        return result.value();
    }
}

void ScriptDataBlock::print() const {
    const std::string compact = toJson();
    rapidjson::Document doc;
    if (!doc.Parse(compact.c_str()).HasParseError()) {
        rapidjson::StringBuffer buf;
        rapidjson::PrettyWriter<rapidjson::StringBuffer> w(buf);
        w.SetIndent(' ', 2);
        doc.Accept(w);
        fmt::print("{}\n", buf.GetString());
    } else {
        fmt::print("{}\n", compact);
    }
}

void ScriptDataBlock::writeFieldToMemory(size_t t_offset, const uint8_t* tp_data, size_t t_len) {
    std::vector<uint8_t> tp_before(t_len, 0);
    const auto before_res = conn_->memory_.readDbMemory(db_num_, t_offset, t_len, tp_before.data());
    const bool have_before = !before_res.hasError();
    if (auto r = conn_->memory_.writeDbMemory(db_num_, t_offset, t_len, tp_data); !r) {
        fmt::print(stderr, "DataBlock::writeFieldToMemory: failed to write to DB{} offset={}: {}\n", db_num_, t_offset, r.error());
        return;
    }
    snapshot_valid_ = true;
    if (have_before)
        markDirtyDiff(conn_->runtime_, db_num_, t_offset, tp_before.data(), tp_data, t_len);
    else if (conn_->runtime_)
        conn_->runtime_->markDirty(db_num_, static_cast<uint32_t>(t_offset), static_cast<uint32_t>(t_len));
}

void ScriptDataBlock::readFieldFromMemory(size_t t_offset, uint8_t* tp_data, size_t t_len) const {
    if (auto r = conn_->memory_.readDbMemory(db_num_, t_offset, t_len, tp_data); !r) {
        fmt::print(stderr, "DataBlock::readFieldFromMemory: failed to read DB{} offset={}: {}\n", db_num_, t_offset, r.error());
    }
}

std::string ScriptDataBlock::diff() const {
    if (!conn_->client_.isConnected())
        return "SclError: not connected";
    auto schema_res = conn_->schema_.getDb(db_num_);
    if (schema_res.hasError())
        return fmt::format("SclError: DB{} not in schema", db_num_);

    DbSnapshot local(*schema_res.value());
    if (auto r = local.read(conn_->client_); r.hasError())
        return fmt::format("SclError: read local failed: {}", toString(r.error()));

    DbSnapshot live(*schema_res.value());
    if (auto r = live.read(conn_->client_); r.hasError())
        return fmt::format("SclError: read live failed: {}", toString(r.error()));

    std::string out = fmt::format("Diff DB{} ({}) local vs live:\n", db_num_, schema_res.value()->db_name);
    bool found = false;

    std::function<void(const std::vector<DbField>&, const std::string&)> diff_fields;
    diff_fields = [&](const std::vector<DbField>& t_fields, const std::string& t_path_prefix) {
        for (const auto& field : t_fields) {
            const std::string field_path = t_path_prefix.empty() ? field.name : t_path_prefix + "." + field.name;
            if (!field.children.empty()) {
                diff_fields(field.children, field_path);
                continue;
            }
            const std::string s_local = shell::valueOr(local.getFieldValue(field_path), std::string{"null"});
            const std::string s_live = shell::valueOr(live.getFieldValue(field_path), std::string{"null"});
            if (s_local != s_live) {
                out += fmt::format("  [{:<30}] {} -> {}\n", field_path, s_local, s_live);
                found = true;
            }
        }
    };
    diff_fields(schema_res.value()->fields, "");

    if (!found)
        out += "No differences.\n";
    return out;
}

// ── Retry helpers ─────────────────────────────────────────────────────

std::string ScriptDataBlock::getRetry(const std::string& t_path, int t_max_retries) {
    if (t_max_retries <= 0)
        t_max_retries = 1;
    // Offline reads are deterministic local twin reads — one attempt through
    // get() suffices (mirrors tags.getRetry()).
    if (!conn_->client_.isConnected())
        return get(t_path);
    for (int attempt = 1; attempt <= t_max_retries; ++attempt) {
        conn_->memory_.processor()->processCommands();
        auto res = conn_->getOrCreateDbProvider(db_num_)->get(conn_->client_, t_path);
        if (!res.hasError()) {
            last_op_ok_ = true;
            snapshot_valid_ = true;
            return res.value();
        }
        last_op_ok_ = false;
        last_op_err_ = res.error();
        if (conn_)
            conn_->setLastError(S7Error::ReadError);
        if (attempt < t_max_retries)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return "null";
}

bool ScriptDataBlock::putRetry(const std::string& t_path, const std::string& t_raw_val, int t_max_retries) {
    if (t_max_retries <= 0)
        t_max_retries = 1;
    // Offline writes are deterministic local twin writes — one attempt
    // through put() suffices (mirrors tags.putRetry()).
    if (!conn_->client_.isConnected()) {
        put(t_path, t_raw_val);
        return getLastOpOk();
    }
    const std::string t_json_val = ::sgrn::gateway::twin::parseRawValuePayload(t_raw_val);
    for (int attempt = 1; attempt <= t_max_retries; ++attempt) {
        auto res = conn_->getOrCreateDbProvider(db_num_)->put(conn_->client_, t_path, t_json_val);
        if (!res.hasError()) {
            last_op_ok_ = true;
            commitBaseline(t_path, t_json_val);
            return true;
        }
        last_op_ok_ = false;
        last_op_err_ = res.error();
        if (conn_)
            conn_->setLastError(S7Error::WriteError);

        if (attempt < t_max_retries)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool ScriptDataBlock::putRetryDouble(const std::string& t_path, double t_val, int t_max_retries) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.Double(t_val);
    // Explicit std::string (see putDouble): bare const char* would bind the
    // putRetry(string, bool, int) overload and retry writing "true".
    return putRetry(t_path, std::string(sb.GetString()), t_max_retries);
}

bool ScriptDataBlock::putRetryInt(const std::string& t_path, int32_t t_val, int t_max_retries) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.Int(t_val);
    // See putRetryDouble: explicit std::string defeats the bool overload.
    return putRetry(t_path, std::string(sb.GetString()), t_max_retries);
}

bool ScriptDataBlock::putRetryBool(const std::string& t_path, bool t_val, int t_max_retries) {
    // Explicit strings: no putRetry(string, bool, int) overload exists, and a
    // literal pair would decay to const char* and misbind.
    return putRetry(t_path, t_val ? std::string("true") : std::string("false"), t_max_retries);
}

} // namespace sgrn::s7shell::shell
