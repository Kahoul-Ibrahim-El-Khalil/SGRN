#pragma once

#include <sgrn/gateway/wrappers/s7/S7Server.hpp>
#include <sgrn/s7shell/runtime/PlcRuntime.hpp>

#include <memory>
#include <string>

namespace sgrn::s7shell::shell
{

class ScriptS7Server : public ::sgrn::gateway::wrappers::s7::S7Server {
public:
    ScriptS7Server(runtime::PlcRuntimeSPtr tsp_rt, const std::string& t_ip, uint16_t t_port);
    ~ScriptS7Server() override;

    void addRef();
    void release();

    runtime::PlcRuntimeSPtr getRuntime() const;

    sgrn::Result<void, ::sgrn::gateway::wrappers::s7::S7Error> startServer();
    void stopServer();

    std::string getIp() const {
        return ip_;
    }
    uint16_t getPort() const {
        return port_;
    }

protected:
    void configureBeforeStart() override;

private:
    static int S7API s7RequestCallback(void* tp_usr_ptr, int t_sender, int t_operation, PS7Tag t_tag, void* tp_data);

    int ref_count_{1};
    runtime::PlcRuntimeSPtr runtime_;
    std::string ip_;
    uint16_t port_;
};

} // namespace sgrn::s7shell::shell
