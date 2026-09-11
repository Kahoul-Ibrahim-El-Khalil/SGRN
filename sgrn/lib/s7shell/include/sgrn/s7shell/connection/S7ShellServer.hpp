#pragma once

#include <sgrn/plcsim/runtime/PlcRuntime.hpp>
#include <sgrn/wrappers/s7/S7Server.hpp>

#include <memory>
#include <string>

namespace sgrn::s7shell::shell
{

class ScriptS7Server : public ::sgrn::wrappers::s7::S7Server {
public:
    ScriptS7Server(::sgrn::plcsim::runtime::PlcRuntimeSPtr tsp_rt, const std::string& t_ip, uint16_t t_port);
    ~ScriptS7Server() override;

    void addRef();
    void release();

    ::sgrn::plcsim::runtime::PlcRuntimeSPtr getRuntime() const;

    sgrn::Result<void, ::sgrn::wrappers::s7::S7Error> startServer();
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
    ::sgrn::plcsim::runtime::PlcRuntimeSPtr runtime_;
    std::string ip_;
    uint16_t port_;
};

} // namespace sgrn::s7shell::shell
