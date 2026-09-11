#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/common/MemoryPort.hpp>
#include <sgrn/common/SecurityPort.hpp>
#include <sgrn/structures/SharedBuffer.hpp>
#include <sgrn/wrappers/s7/S7Server.hpp>
#include <sgrn/wrappers/s7/error.hpp>

#include <map>
#include <memory>
#include <optional>
#include <snap7.h>
#include <utility>
#include <vector>

namespace sgrn::gateway::adapters::s7
{

using ::sgrn::common::IMemoryPort;
using ::sgrn::common::ISecurityPolicy;
using ::sgrn::wrappers::s7::S7Error;
using ::sgrn::wrappers::s7::S7Server;

class S7ProtocolAdapter : public S7Server {
public:
    S7ProtocolAdapter(IMemoryPort& t_memory, std::shared_ptr<ISecurityPolicy> tsp_security_policy);
    ~S7ProtocolAdapter() override;

    void setMaxClientsConfig(size_t t_max_clients) {
        max_clients_ = t_max_clients;
    }
    void setPduSizeConfig(size_t t_pdu_size) {
        pdu_size_ = t_pdu_size;
    }

    void configureBeforeStart() override;
    sgrn::Result<void, S7Error> bindToPlcMemory();
    bool isRequestInSemanticSpace(const TS7Tag& t_tag) const;
    sgrn::Result<void, S7Error> registerSemanticArea(int t_area_code, word t_index, size_t t_size);

    static int S7API s7RequestCallback(void* tp_usr_ptr, int t_sender, int t_operation, PS7Tag t_tag, void* tp_data);

private:
    IMemoryPort& memory_;
    std::shared_ptr<ISecurityPolicy> security_policy_;
    std::map<std::pair<int, word>, std::shared_ptr<sgrn::SharedBuffer>> area_buffers_;
    std::map<std::pair<int, word>, std::vector<std::pair<int, int>>> semantic_spans_;
    size_t max_clients_{1024};
    size_t pdu_size_{480};
};

} // namespace sgrn::gateway::adapters::s7
