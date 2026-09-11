#pragma once

#include <string>

namespace sgrn::s7shell::shell
{

class ScriptDataBlock;

class ScriptHexTable {
public:
    explicit ScriptHexTable(ScriptDataBlock* tp_db);
    ~ScriptHexTable();

    void addRef();
    void release();

    void print() const;
    std::string toString() const;

private:
    int ref_count_{1};
    ScriptDataBlock* db_{nullptr};
};

ScriptHexTable* DataBlockToHexTableCast(ScriptDataBlock* tp_db);

} // namespace sgrn::s7shell::shell
