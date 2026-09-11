#include <sgrn/s7shell/script/ScriptSchemaStore.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>

#include <fmt/format.h>

namespace sgrn::s7shell::shell
{

ScriptSchemaStore::ScriptSchemaStore(::sgrn::scl::PlcSchemaStore* tp_schema)
    : schema_(tp_schema) {
}

void ScriptSchemaStore::addRef() {
    ++ref_count_;
}

void ScriptSchemaStore::release() {
    if (--ref_count_ == 0)
        delete this;
}

void ScriptSchemaStore::print() {
    if (!schema_)
        return;
    std::string json = schema_->toJson(std::nullopt, false, true);
    fmt::print("{}\n", json);
}

} // namespace sgrn::s7shell::shell
