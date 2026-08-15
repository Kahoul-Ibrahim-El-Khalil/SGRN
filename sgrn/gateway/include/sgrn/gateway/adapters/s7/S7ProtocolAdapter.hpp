#pragma once
#include <sgrn/gateway/wrappers/s7/ProtocolError.hpp>
#include <sgrn/gateway/wrappers/s7/S7Server.hpp>
#include <sgrn/scl/types.hpp>
#include <sgrn/structures/SharedBuffer.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
namespace sgrn::gateway::twin
{
class PlcMemory;

} // namespace sgrn::gateway::twin

namespace sgrn::gateway
{
class SecurityManager;
} // namespace sgrn::gateway

namespace sgrn::gateway::adapters::s7
{

using ::sgrn::gateway::wrappers::s7::S7Error;
using ::sgrn::gateway::wrappers::s7::S7ProtocolCode;
using ::sgrn::gateway::wrappers::s7::S7Server;
using ::sgrn::scl::SecurityPolicy;

class S7ProtocolAdapter : public S7Server {
public:
    S7ProtocolAdapter(
        ::sgrn::gateway::twin::PlcMemory& t_plc_memory, std::shared_ptr<::sgrn::gateway::SecurityManager> tsp_security_manager);
    ~S7ProtocolAdapter() override;

    S7ProtocolAdapter(const S7ProtocolAdapter&) = delete;
    S7ProtocolAdapter& operator=(const S7ProtocolAdapter&) = delete;

    void setMaxClientsConfig(uint32_t t_max_clients) {
        max_clients_ = t_max_clients;
    }
    void setPduSizeConfig(uint32_t t_pdu_size) {
        pdu_size_ = t_pdu_size;
    }

    sgrn::Result<void, S7Error> bindToPlcMemory();

protected:
    void configureBeforeStart() override;

private:
    static int S7API s7RequestCallback(void* tp_usr_ptr, int t_sender, int t_operation, PS7Tag t_tag, void* tp_data);
    bool isRequestInSemanticSpace(const TS7Tag& t_tag) const;
    sgrn::Result<void, S7Error> registerSemanticArea(int t_area_code, word t_index, size_t t_size);

    ::sgrn::gateway::twin::PlcMemory& plc_memory_;

    std::shared_ptr<::sgrn::gateway::SecurityManager> security_manager_;
    uint32_t max_clients_{8};
    uint32_t pdu_size_{960};

    std::map<std::pair<int, word>, std::shared_ptr<sgrn::SharedBuffer>> area_buffers_;
    std::map<std::pair<int, word>, std::vector<std::pair<int, int>>> semantic_spans_;
};

} // namespace sgrn::gateway::adapters::s7
