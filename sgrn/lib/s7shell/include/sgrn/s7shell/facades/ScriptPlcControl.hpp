#pragma once

#include <string>

namespace sgrn::s7shell::shell
{

struct ScriptS7Connection;

class ScriptS7PlcControl {
public:
    explicit ScriptS7PlcControl(ScriptS7Connection* tp_conn);

    void addRef();
    void release();

    void hotStart();
    void coldStart();
    void stop();

    std::string clock() const;
    void setClock(int t_year, int t_month, int t_day, int t_hour, int t_minute, int t_second);
    void syncClockToSystem();

    void setPassword(const std::string& t_password);
    void clearPassword();

    void copyRamToRom(int t_timeout_ms);
    void compress(int t_timeout_ms);

private:
    int ref_count_{1};
    ScriptS7Connection* conn_{nullptr};
};

} // namespace sgrn::s7shell::shell
