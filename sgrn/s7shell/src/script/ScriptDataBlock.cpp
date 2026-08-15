#include <sgrn/gateway/twin/twin.hpp>
#include <sgrn/s7shell/connection/S7Connection.hpp>
#include <sgrn/s7shell/runtime/PlcRuntime.hpp>
#include <sgrn/s7shell/script/ScriptDataBlock.hpp>
#include <sgrn/s7shell/script/ScriptFieldProxy.hpp>
#include <sgrn/s7shell/script/ScriptPathBatch.hpp>
#include <sgrn/s7shell/script/ScriptTagTable.hpp>
#include <sgrn/s7shell/utils/json_helpers.hpp>
#include <sgrn/scl/types.hpp>

#include <fmt/color.h>
#include <fmt/format.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <thread>

namespace sgrn::s7shell::shell
{

using namespace sgrn::scl;
using namespace ::sgrn::gateway::twin;

static void markDirtyDiff(const runtime::PlcRuntimeSPtr& tsp_runtime, uint16_t t_db_num, size_t t_base_offset, const uint8_t* tp_before,
    const uint8_t* tp_after, size_t t_len) {
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

void ScriptDataBlock::notifyConnError(const ::sgrn::scl::Error& t_err) {
    if (conn_) {
        conn_->setLastError(t_err);
    }
}

void ScriptDataBlock::notifyConnError(const ::sgrn::gateway::wrappers::s7::S7Error& t_err) {
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
        shell::logError(::sgrn::scl::Error(SchemaCode::Generic, "No schema loaded and DB size not registered"), "DataBlock::get");
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
        }
        last_op_ok_ = true;
        last_op_err_.clear();
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
        last_op_err_ = std::string(toString(r.error().status()));
        addRef();
        return this;
    }
    snapshot_valid_ = true;
    // Persist as shared baseline: future db() instances for this DB number
    // start from confirmed PLC data, not a stale or zero-init buffer.
    conn_->db_snapshots_[db_num_] = snapshot_buffer_;
    last_op_ok_ = true;
    last_op_err_.clear();
    addRef();
    return this;
}

void ScriptDataBlock::push() {
    if (db_size_ == 0) {
        shell::logError(::sgrn::scl::Error(SchemaCode::Generic, "No schema loaded and DB size not registered"), "DataBlock::push");
        return;
    }

    // Flush any pending write commands to the local memory arena
    conn_->memory_.processor()->processCommands();

    std::vector<uint8_t> current_mem(db_size_);
    if (!(conn_->memory_.readDbMemory(db_num_, 0, db_size_, current_mem.data()))) {
        fmt::print(stderr, "DataBlock::push: failed to read local memory for DB{}\n", db_num_);
        return;
    }

    // If not connected to a real PLC, update the snapshot so getters see the new
    // values (write-through to snapshot = "commit to local memory").
    if (!conn_->client_.isConnected()) {
        snapshot_buffer_ = current_mem;
        conn_->db_snapshots_[db_num_] = current_mem;
        last_op_ok_ = true;
        last_op_err_.clear();
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
                    shell::logError(res.error(), fmt::format("DB{}.push (segment @{}, len {})", db_num_, start, i - start));
                    (void)setOpResult(res);
                    write_failed = true;
                }
                in_dirty_region = false;
            }
        }
    }
    if (in_dirty_region && !write_failed) {
        auto res = conn_->client_.writeArea(
            S7AreaDB, db_num_, static_cast<int>(start), static_cast<int>(db_size_ - start), S7WLByte, current_mem.data() + start);
        if (res.hasError()) {
            shell::logError(res.error(), fmt::format("DB{}.push (segment @{}, len {})", db_num_, start, db_size_ - start));
            (void)setOpResult(res);
            write_failed = true;
        }
    }
    if (write_failed) {
        fmt::print(stderr, fg(fmt::color::red), "[S7] DB{}: push aborted — PLC write failed, snapshot NOT updated\n", db_num_);
        return;
    }

    // Update snapshot to current state
    snapshot_buffer_ = current_mem;
    conn_->db_snapshots_[db_num_] = current_mem;
    last_op_ok_ = true;
    last_op_err_.clear();
}

void ScriptDataBlock::write(const std::string& t_path, const std::string& t_raw_val) {
    const std::string t_json_val = ::sgrn::gateway::twin::parseRawValuePayload(t_raw_val);
    // Encode directly into the memory arena using the authoritative schema field.
    // This bypasses the async PlcCommand queue whose plcNodeToDbField() reconstruction
    // is unreliable for complex types (DTL, nested structs). Using schema.findField()
    // is the same path as put() — guarantees push() detects dirty bytes correctly.
    const auto loc = conn_->schema_.findField(db_num_, t_path);
    if (!loc) {
        shell::logError(::sgrn::scl::Error(SchemaCode::NotFound, fmt::format("DB{}: field '{}' not found in schema", db_num_, t_path)),
            fmt::format("DB{}.write", db_num_));
        return;
    }
    ::sgrn::scl::DbField target_field = *loc->field;
    if (!t_path.empty() && t_path.back() == ']') {
        target_field.count = 1;
    }
    const int span = ::sgrn::gateway::twin::fieldSpanSize(target_field);
    if (span <= 0)
        return;
    // Read existing bytes first so boolean bits in shared bytes are preserved,
    // and so runtime dirty tracking can record the actual changed bytes.
    std::vector<uint8_t> tp_before(static_cast<size_t>(span), 0);
    std::vector<uint8_t> buf(static_cast<size_t>(span), 0);
    if (!(conn_->memory_.readDbMemory(db_num_, loc->abs_offset, tp_before.size(), tp_before.data()))) {
        shell::logError(::sgrn::scl::Error(SchemaCode::Generic, "readDbMemory failed"), fmt::format("DB{}.read('{}')", db_num_, t_path));
        return;
    }
    buf = tp_before;
    auto res =
        ::sgrn::gateway::twin::encodeFieldAt(target_field, t_json_val, buf.data(), static_cast<size_t>(span), 0, target_field.endianness);
    if (res.hasError()) {
        shell::logError(res.error(), fmt::format("DB{}.write('{}')", db_num_, t_path));
        return;
    }
    if (auto r = conn_->memory_.writeDbMemory(db_num_, loc->abs_offset, buf.size(), buf.data()); !r) {
        shell::logError(::sgrn::scl::Error(SchemaCode::Generic, fmt::format("writeDbMemory failed: {}", r.error())),
            fmt::format("DB{}.write('{}')", db_num_, t_path));
        return;
    }
    snapshot_valid_ = true;
    markDirtyDiff(conn_->runtime_, db_num_, loc->abs_offset, tp_before.data(), buf.data(), buf.size());
}

void ScriptDataBlock::writeScalar(const std::string& t_path, const s7codec::DecodedValue& t_val) {
    if (!conn_->runtime_)
        return;
    auto loc = conn_->runtime_->getSchema().findField(db_num_, t_path);
    if (!loc) {
        shell::logError(::sgrn::scl::Error(SchemaCode::NotFound, fmt::format("Field not found: {}", t_path)),
            fmt::format("DB{}.writeScalar('{}')", db_num_, t_path));
        return;
    }

    int sz = (loc->field->type == DataType::Bool && loc->field->count <= 1) ? 1 : ::sgrn::gateway::twin::fieldSpanSize(*loc->field);
    std::vector<uint8_t> buf(sz, 0);

    // Read existing bytes first to preserve other bits (especially for bit-packed BOOL arrays)
    readFieldFromMemory(loc->abs_offset, buf.data(), sz);

    auto status =
        s7codec::encodeScalar(t_val, loc->field->type, buf.data(), sz, loc->field->bit_index, loc->field->count, loc->field->endianness);
    if (!status.ok()) {
        shell::logError(
            ::sgrn::scl::Error(SchemaCode::InvalidType, status.message), fmt::format("DB{}.writeScalar('{}')", db_num_, t_path));
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

void ScriptDataBlock::writeDict(const std::string& t_path, void* tp_dict) {
    write(t_path, shell::convertDictToJson(static_cast<CScriptDictionary*>(tp_dict)));
}

void ScriptDataBlock::writeArray(const std::string& t_path, void* tp_arr) {
    write(t_path, shell::convertArrayToJson(static_cast<CScriptArray*>(tp_arr)));
}

std::string ScriptDataBlock::val(const std::string& t_path) {
    conn_->memory_.processor()->processCommands();
    if (!snapshot_valid_) {
        auto refresh = conn_->getOrCreateDbProvider(db_num_)->get(conn_->client_, t_path);
        if (refresh.hasError()) {
            shell::logError(refresh.error(), fmt::format("DB{}.val('{}') [S7 read]", db_num_, t_path));
            return "null";
        }
        snapshot_valid_ = true;
    }
    auto res = conn_->getOrCreateDbProvider(db_num_)->read(t_path);
    if (res.hasError()) {
        shell::logError(res.error(), fmt::format("DB{}.val('{}') [decode]", db_num_, t_path));
        return "null";
    }
    return res.value();
}

void ScriptDataBlock::setVal(const std::string& t_path, const std::string& t_json_val) {
    shell::ok(conn_->getOrCreateDbProvider(db_num_)->write(t_path, t_json_val), fmt::format("DB{}.setVal('{}')", db_num_, t_path));
}

std::string ScriptDataBlock::get(const std::string& t_path) {
    conn_->memory_.processor()->processCommands();
    auto res = conn_->getOrCreateDbProvider(db_num_)->get(conn_->client_, t_path);
    if (res.hasError()) {
        shell::logError(res.error(), fmt::format("DB{}.get('{}')", db_num_, t_path));
        last_op_ok_ = false;
        last_op_err_ = res.error().string();
        if (conn_)
            conn_->setLastError(res.error());
        return "null";
    }
    snapshot_valid_ = true;
    last_op_ok_ = true;
    last_op_err_.clear();
    return res.value();
}

s7codec::DecodedValue ScriptDataBlock::readScalar(const std::string& t_path) {
    if (!conn_->runtime_)
        return {};
    auto loc = conn_->runtime_->getSchema().findField(db_num_, t_path);
    if (!loc) {
        shell::logError(::sgrn::scl::Error(SchemaCode::NotFound, fmt::format("Field not found: {}", t_path)),
            fmt::format("DB{}.readScalar('{}')", db_num_, t_path));
        return {};
    }

    // Ensure the snapshot is fetched if it hasn't been yet
    conn_->memory_.processor()->processCommands();
    if (!snapshot_valid_) {
        auto refresh = conn_->getOrCreateDbProvider(db_num_)->get(conn_->client_, t_path);
        if (refresh.hasError()) {
            shell::logError(refresh.error(), fmt::format("DB{}.readScalar('{}') [S7 read]", db_num_, t_path));
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

void ScriptDataBlock::put(const std::string& t_path, const std::string& t_raw_val) {
    const std::string t_json_val = ::sgrn::gateway::twin::parseRawValuePayload(t_raw_val);
    auto res = conn_->getOrCreateDbProvider(db_num_)->put(conn_->client_, t_path, t_json_val);
    if (!setOpResult(res)) {
        shell::logError(res.error(), fmt::format("DB{}.put('{}', '{}')", db_num_, t_path, t_raw_val));
        return;
    }
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
}

void ScriptDataBlock::putDouble(const std::string& t_path, double t_val) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.Double(t_val);
    put(t_path, sb.GetString());
}

void ScriptDataBlock::putInt(const std::string& t_path, int32_t t_val) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.Int(t_val);
    put(t_path, sb.GetString());
}

void ScriptDataBlock::putBool(const std::string& t_path, bool t_val) {
    put(t_path, t_val ? "true" : "false");
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
    return shell::valueOr(conn_->memory_.getSubtreeJson(db_num_, ""), std::string{"{}"});
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
        return "Error: not connected";
    auto schema_res = conn_->schema_.getDb(db_num_);
    if (schema_res.hasError())
        return fmt::format("Error: DB{} not in schema", db_num_);

    DbSnapshot local(*schema_res.value());
    if (auto r = local.read(conn_->client_); r.hasError())
        return fmt::format("Error: read local failed: {}", r.error().string());

    DbSnapshot live(*schema_res.value());
    if (auto r = live.read(conn_->client_); r.hasError())
        return fmt::format("Error: read live failed: {}", r.error().string());

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
    for (int attempt = 1; attempt <= t_max_retries; ++attempt) {
        conn_->memory_.processor()->processCommands();
        auto res = conn_->getOrCreateDbProvider(db_num_)->get(conn_->client_, t_path);
        if (!res.hasError()) {
            last_op_ok_ = true;
            last_op_err_.clear();
            snapshot_valid_ = true;
            return res.value();
        }
        last_op_ok_ = false;
        last_op_err_ = res.error().string();
        if (conn_)
            conn_->setLastError(res.error());
        fmt::print(stderr, fg(fmt::color::yellow), "[S7] DB{}.getRetry('{}') attempt {}/{} failed: {}\n", db_num_, t_path, attempt,
            t_max_retries, res.error().string());
        if (attempt < t_max_retries)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return "null";
}

bool ScriptDataBlock::putRetry(const std::string& t_path, const std::string& t_raw_val, int t_max_retries) {
    if (t_max_retries <= 0)
        t_max_retries = 1;
    const std::string t_json_val = ::sgrn::gateway::twin::parseRawValuePayload(t_raw_val);
    for (int attempt = 1; attempt <= t_max_retries; ++attempt) {
        auto res = conn_->getOrCreateDbProvider(db_num_)->put(conn_->client_, t_path, t_json_val);
        if (!res.hasError()) {
            last_op_ok_ = true;
            last_op_err_.clear();
            if (auto loc = conn_->schema_.findField(db_num_, t_path)) {
                (void)::sgrn::gateway::twin::encodeFieldAt(*loc->field, t_json_val, snapshot_buffer_.data() + loc->abs_offset,
                    db_size_ - loc->abs_offset, 0, loc->field->endianness);
                conn_->db_snapshots_[db_num_] = snapshot_buffer_;
            }
            return true;
        }
        last_op_ok_ = false;
        last_op_err_ = res.error().string();
        if (conn_)
            conn_->setLastError(res.error());
        fmt::print(stderr, fg(fmt::color::yellow), "[S7] DB{}.putRetry('{}') attempt {}/{} failed: {}\n", db_num_, t_path, attempt,
            t_max_retries, res.error().string());
        if (attempt < t_max_retries)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

bool ScriptDataBlock::putRetryDouble(const std::string& t_path, double t_val, int t_max_retries) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.Double(t_val);
    return putRetry(t_path, sb.GetString(), t_max_retries);
}

bool ScriptDataBlock::putRetryInt(const std::string& t_path, int32_t t_val, int t_max_retries) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.Int(t_val);
    return putRetry(t_path, sb.GetString(), t_max_retries);
}

bool ScriptDataBlock::putRetryBool(const std::string& t_path, bool t_val, int t_max_retries) {
    return putRetry(t_path, t_val ? "true" : "false", t_max_retries);
}

} // namespace sgrn::s7shell::shell
