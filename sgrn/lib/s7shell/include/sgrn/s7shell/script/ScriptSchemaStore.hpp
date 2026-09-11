#pragma once

namespace sgrn::scl
{
class PlcSchemaStore;
}

namespace sgrn::s7shell::shell
{

class ScriptSchemaStore {
public:
    explicit ScriptSchemaStore(::sgrn::scl::PlcSchemaStore* tp_schema);

    void addRef();
    void release();

    void print();

private:
    int ref_count_{1};
    ::sgrn::scl::PlcSchemaStore* schema_{nullptr};
};

} // namespace sgrn::s7shell::shell
