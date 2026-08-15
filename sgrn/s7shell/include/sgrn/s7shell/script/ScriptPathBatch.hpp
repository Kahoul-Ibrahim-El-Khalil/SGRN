#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <sgrn/gateway/twin/DbIOProvider.hpp>
#include <sgrn/s7shell/PlcTagTable.hpp>
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

class S7PathBatch {
public:
    explicit S7PathBatch(ScriptDataBlock* tp_db);
    explicit S7PathBatch(ScriptTagTable* tp_tags);
    ~S7PathBatch();

    void addRef();
    void release();

    S7PathBatch* path(const std::string& t_p);
    S7PathBatch* write(const std::string& t_json_val);
    S7PathBatch* writeDouble(double t_val);
    S7PathBatch* writeInt(int32_t t_val);
    S7PathBatch* writeBool(bool t_val);
    S7PathBatch* writeDict(void* tp_dict);
    S7PathBatch* writeArray(void* tp_arr);

    std::string read() const;
    void put();
    void get();
    std::string toJson() const;

private:
    int ref_count_{1};
    ScriptS7Connection* conn_{nullptr};
    ScriptDataBlock* db_{nullptr};
    ScriptTagTable* tags_{nullptr};
    using EngineVariant = std::variant<std::unique_ptr<::sgrn::s7shell::S7BatchEngine<::sgrn::gateway::twin::DbIOProvider>>,
        std::unique_ptr<::sgrn::s7shell::S7BatchEngine<::sgrn::s7shell::PlcTagTable>>>;
    EngineVariant engine_;
};

} // namespace sgrn::s7shell::shell
