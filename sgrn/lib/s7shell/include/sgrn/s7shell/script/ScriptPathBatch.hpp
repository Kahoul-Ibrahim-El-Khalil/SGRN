#pragma once

#include <sgrn/s7shell/script/AngelScriptObject.hpp>
#include <cstdint>
#include <memory>
#include <string>

#include <sgrn/gateway/twin/DbIOProvider.hpp>
#include <sgrn/plcsim/PlcTagTable.hpp>
#include <sgrn/s7shell/S7BatchEngine.hpp>
#include <variant>

namespace sgrn::s7shell
{
}

namespace sgrn::s7shell::shell
{

class ScriptDataBlock;
class ScriptTagTable;
struct ScriptS7Connection;

class S7PathBatch : public AngelScriptObject {
public:
    explicit S7PathBatch(ScriptDataBlock* tp_db);
    explicit S7PathBatch(ScriptTagTable* tp_tags);
    ~S7PathBatch() override;

    S7PathBatch* path(const std::string& t_p);

    std::string read() const;
    void put();
    void get();
    std::string toJson() const;

private:
    ScriptS7Connection* conn_{nullptr};
    ScriptDataBlock* db_{nullptr};
    ScriptTagTable* tags_{nullptr};
    using EngineVariant = std::variant<std::unique_ptr<::sgrn::s7shell::S7BatchEngine<::sgrn::gateway::twin::DbIOProvider>>,
        std::unique_ptr<::sgrn::s7shell::S7BatchEngine<::sgrn::plcsim::PlcTagTable>>>;
    EngineVariant engine_;
};

} // namespace sgrn::s7shell::shell
