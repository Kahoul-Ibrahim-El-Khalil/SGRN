#pragma once

#include <sgrn/s7shell/script/AngelScriptObject.hpp>
#include <string>

namespace sgrn::s7shell::shell
{

class ScriptDataBlock;

class ScriptHexTable : public AngelScriptObject {
public:
    explicit ScriptHexTable(ScriptDataBlock* tp_db);
    ~ScriptHexTable() override;

    void print() const;
    std::string toString() const;

private:
    ScriptDataBlock* db_{nullptr};
};

ScriptHexTable* DataBlockToHexTableCast(ScriptDataBlock* tp_db);

} // namespace sgrn::s7shell::shell
