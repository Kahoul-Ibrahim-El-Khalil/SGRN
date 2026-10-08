#include <sgrn/s7shell/script/ScriptSchemaStore.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>

#include <fmt/format.h>
#include <stdexcept>
#include <string>

namespace sgrn::s7shell::shell
{

ScriptSchemaStore::ScriptSchemaStore(::sgrn::scl::PlcSchemaStore* tp_schema)
    : schema_(tp_schema) {
}

void ScriptSchemaStore::print() {
    if (!schema_)
        return;
    std::string json = schema_->toJson(std::nullopt, false, true);
    fmt::print("{}\n", json);
}

} // namespace sgrn::s7shell::shell
