#pragma once

#include <sgrn/s7shell/script/AngelScriptObject.hpp>

namespace sgrn::scl
{
class PlcSchemaStore;
}

namespace sgrn::s7shell::shell
{

class ScriptSchemaStore : public AngelScriptObject {
public:
    explicit ScriptSchemaStore(::sgrn::scl::PlcSchemaStore* tp_schema);

    void print();

private:
    ::sgrn::scl::PlcSchemaStore* schema_{nullptr};
};

} // namespace sgrn::s7shell::shell
