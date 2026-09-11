#pragma once

#include <cstdint>
#include <string>

namespace sgrn::s7shell::shell
{

struct ScriptS7Connection;

class ScriptS7Diagnostics {
public:
    explicit ScriptS7Diagnostics(ScriptS7Connection* tp_conn);

    void addRef();
    void release();

    // Level 0 — connection / transport
    std::string connectionInfo() const;
    int lastError() const;
    std::string lastErrorText() const;
    std::string pduInfo() const;

    // Level 1 — CPU run state
    std::string status() const;
    bool isRunning() const;

    // Level 2 — module / communication processor
    std::string cpuInfo() const;
    std::string orderCode() const;
    std::string cpInfo() const;
    std::string info() const;

    // Level 3 — diagnostic buffer & SZL
    std::string diagnosticBuffer(int t_count = 10) const;
    std::string szl(int t_id, int t_index) const;

    // Level 4 — block directory (PG view)
    std::string listBlocks() const;
    std::string listBlocksOfType(int t_block_type) const;
    std::string blockInfo(int t_block_type, uint16_t t_block_number) const;

    // Level 5 — protection (read-only here; writes on S7PlcControl)
    std::string protection() const;

private:
    int ref_count_{1};
    ScriptS7Connection* conn_{nullptr};
};

} // namespace sgrn::s7shell::shell
