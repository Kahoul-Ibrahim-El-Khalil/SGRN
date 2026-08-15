#pragma once

#include <cstdint>
#include <string>

namespace sgrn::s7shell::shell
{

struct ScriptS7Connection;

class ScriptS7ConnectionProxy {
public:
    explicit ScriptS7ConnectionProxy(ScriptS7Connection* tp_conn);

    void addRef();
    void release();

    bool connectWithTsap(const std::string& t_ip, uint16_t t_local_tsap, uint16_t t_remote_tsap);
    void useTsap(uint16_t t_local_tsap, uint16_t t_remote_tsap);
    void useRackSlot();

    bool usesTsap() const;
    uint16_t localTsap() const;
    uint16_t remoteTsap() const;

    int getParamInt(int t_param) const;
    void setParamInt(int t_param, int t_value);

    uint16_t getParamUInt16(int t_param) const;
    void setParamUInt16(int t_param, uint16_t t_value);

    std::string paramSummary() const;

private:
    int ref_count_{1};
    ScriptS7Connection* conn_{nullptr};
};

} // namespace sgrn::s7shell::shell
