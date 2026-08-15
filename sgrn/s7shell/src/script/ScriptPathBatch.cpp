#include <sgrn/s7shell/S7BatchEngine.hpp>
#include <sgrn/s7shell/connection/S7Connection.hpp>
#include <sgrn/s7shell/script/ScriptDataBlock.hpp>
#include <sgrn/s7shell/script/ScriptPathBatch.hpp>
#include <sgrn/s7shell/script/ScriptTagTable.hpp>
#include <sgrn/s7shell/utils/json_helpers.hpp>

#include <fmt/format.h>
#include <scriptarray/scriptarray.h>
#include <scriptdictionary/scriptdictionary.h>

namespace sgrn::s7shell::shell
{

S7PathBatch::S7PathBatch(ScriptDataBlock* tp_db)
    : conn_(tp_db->conn_)
    , db_(tp_db)
    , engine_(std::make_unique<::sgrn::s7shell::S7BatchEngine<::sgrn::gateway::twin::DbIOProvider>>(
          *conn_->getOrCreateDbProvider(tp_db->getDbNumber()))) {
    if (db_)
        db_->addRef();
}

S7PathBatch::S7PathBatch(ScriptTagTable* tp_tags)
    : conn_(tp_tags->conn_)
    , db_(nullptr)
    , tags_(tp_tags)
    , engine_(conn_->tag_table_ ? std::make_unique<::sgrn::s7shell::S7BatchEngine<::sgrn::s7shell::PlcTagTable>>(*conn_->tag_table_)
                                : nullptr) {
    if (tags_)
        tags_->addRef();
}

S7PathBatch::~S7PathBatch() {
    if (db_)
        db_->release();
    if (tags_)
        tags_->release();
}

void S7PathBatch::addRef() {
    ++ref_count_;
}

void S7PathBatch::release() {
    if (--ref_count_ == 0)
        delete this;
}

S7PathBatch* S7PathBatch::path(const std::string& t_p) {
    if (std::visit([&](const auto& e) { return e == nullptr; }, engine_))
        return this;
    std::visit([&](auto& e) { e->path(t_p); }, engine_);
    addRef();
    return this;
}

S7PathBatch* S7PathBatch::write(const std::string& t_json_val) {
    if (std::visit([&](const auto& e) { return e == nullptr; }, engine_))
        return this;
    std::visit([&](auto& e) { e->write(t_json_val); }, engine_);
    if (std::visit([&](auto& e) { return e->hasError(); }, engine_)) {
        shell::logError(std::visit([&](auto& e) { return e->getLastError(); }, engine_));
    }
    addRef();
    return this;
}

S7PathBatch* S7PathBatch::writeDouble(double t_val) {
    if (std::visit([&](const auto& e) { return e == nullptr; }, engine_))
        return this;
    std::visit([&](auto& e) { e->write(t_val); }, engine_);
    if (std::visit([&](auto& e) { return e->hasError(); }, engine_)) {
        shell::logError(std::visit([&](auto& e) { return e->getLastError(); }, engine_));
    }
    addRef();
    return this;
}

S7PathBatch* S7PathBatch::writeInt(int32_t t_val) {
    if (std::visit([&](const auto& e) { return e == nullptr; }, engine_))
        return this;
    std::visit([&](auto& e) { e->write(t_val); }, engine_);
    if (std::visit([&](auto& e) { return e->hasError(); }, engine_)) {
        shell::logError(std::visit([&](auto& e) { return e->getLastError(); }, engine_));
    }
    addRef();
    return this;
}

S7PathBatch* S7PathBatch::writeBool(bool t_val) {
    if (std::visit([&](const auto& e) { return e == nullptr; }, engine_))
        return this;
    std::visit([&](auto& e) { e->write(t_val); }, engine_);
    if (std::visit([&](auto& e) { return e->hasError(); }, engine_)) {
        shell::logError(std::visit([&](auto& e) { return e->getLastError(); }, engine_));
    }
    addRef();
    return this;
}

S7PathBatch* S7PathBatch::writeDict(void* tp_dict) {
    if (std::visit([&](const auto& e) { return e == nullptr; }, engine_))
        return this;
    std::visit([&](auto& e) { e->write(shell::convertDictToJson(static_cast<CScriptDictionary*>(tp_dict))); }, engine_);
    if (std::visit([&](auto& e) { return e->hasError(); }, engine_)) {
        shell::logError(std::visit([&](auto& e) { return e->getLastError(); }, engine_));
    }
    addRef();
    return this;
}

S7PathBatch* S7PathBatch::writeArray(void* tp_arr) {
    if (std::visit([&](const auto& e) { return e == nullptr; }, engine_))
        return this;
    std::visit([&](auto& e) { e->write(shell::convertArrayToJson(static_cast<CScriptArray*>(tp_arr))); }, engine_);
    if (std::visit([&](auto& e) { return e->hasError(); }, engine_)) {
        shell::logError(std::visit([&](auto& e) { return e->getLastError(); }, engine_));
    }
    addRef();
    return this;
}

std::string S7PathBatch::read() const {
    if (std::visit([&](const auto& e) { return e == nullptr; }, engine_))
        return "null";
    return shell::valueOr(std::visit([&](auto& e) { return e->read(); }, engine_), std::string{"null"});
}

void S7PathBatch::put() {
    if (std::visit([&](const auto& e) { return e == nullptr; }, engine_))
        return;
    if (db_) {
        // Refactored to use writeArea via DataBlock::push
        // 1. Apply batch to local shadow memory
        (void)shell::ok(std::visit([&](auto& e) { return e->commitLocal(); }, engine_));
        // 2. Push dirty regions to PLC using writeArea
        db_->push();
    } else {
        // Fallback to MultiVar for TagTable batches
        (void)shell::ok(std::visit([&](auto& e) { return e->put(conn_->client_); }, engine_));
    }
    std::visit([&](auto& e) { e->reset(); }, engine_);
}

void S7PathBatch::get() {
    if (std::visit([&](const auto& e) { return e == nullptr; }, engine_))
        return;
    (void)shell::ok(std::visit([&](auto& e) { return e->get(conn_->client_); }, engine_));
    std::visit([&](auto& e) { e->reset(); }, engine_);
}

std::string S7PathBatch::toJson() const {
    if (std::visit([&](const auto& e) { return e == nullptr; }, engine_))
        return "{}";
    return shell::valueOr(std::visit([&](auto& e) { return e->toJson(); }, engine_), std::string{"{}"});
}

} // namespace sgrn::s7shell::shell
