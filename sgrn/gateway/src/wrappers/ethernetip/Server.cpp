#include <sgrn/gateway/wrappers/ethernetip/Server.hpp>

#include <fmt/core.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <cstdlib>
#include <unordered_map>
#include <utility>

extern "C" {
#include <appcontype.h>
#include <cip/cipcommon.h>
#include <cip/cipidentity.h>
#include <cip/cipqos.h>
#include <cip/ciptcpipinterface.h>
#include <ports/generic_networkhandler.h>
}

namespace sgrn::gateway::wrappers::ethernetip
{

namespace
{

PreGetCallback g_pre_get;
PostSetCallback g_post_set;
CipAttributeDecodeFromMessage g_byte_array_decoder = nullptr;

extern "C" EipStatus wrapperPreGetCallback(CipInstance* const tp_instance, CipAttributeStruct* const tp_attribute, CipByte t_service) {
    if (!g_pre_get)
        return kEipStatusError;
    return g_pre_get(tp_instance, tp_attribute, t_service);
}

extern "C" EipStatus wrapperPostSetCallback(CipInstance* const tp_instance, CipAttributeStruct* const tp_attribute, CipByte t_service) {
    if (!g_post_set)
        return kEipStatusError;
    return g_post_set(tp_instance, tp_attribute, t_service);
}

std::unordered_map<uint16_t, CipInstance*>& instanceTable() {
    static std::unordered_map<uint16_t, CipInstance*> table;
    return table;
}

} // namespace

Server::Server(ServerConfig t_cfg)
    : cfg_(std::move(t_cfg)) {
}

Server::~Server() noexcept {
    destroy();
}

Server::Server(Server&& t_other) noexcept
    : cfg_(std::move(t_other.cfg_))
    , vendor_class_(std::exchange(t_other.vendor_class_, nullptr))
    , pre_get_(std::move(t_other.pre_get_))
    , post_set_(std::move(t_other.post_set_))
    , byte_array_decoder_(t_other.byte_array_decoder_)
    , shutdown_requested_(t_other.shutdown_requested_.load())
    , stack_initialized_(t_other.stack_initialized_)
    , network_started_(t_other.network_started_) {
    t_other.stack_initialized_ = false;
    t_other.network_started_ = false;
}

Server& Server::operator=(Server&& t_other) noexcept {
    if (this != &t_other) {
        destroy();
        cfg_ = std::move(t_other.cfg_);
        vendor_class_ = std::exchange(t_other.vendor_class_, nullptr);
        pre_get_ = std::move(t_other.pre_get_);
        post_set_ = std::move(t_other.post_set_);
        byte_array_decoder_ = t_other.byte_array_decoder_;
        shutdown_requested_.store(t_other.shutdown_requested_.load());
        stack_initialized_ = t_other.stack_initialized_;
        network_started_ = t_other.network_started_;
        t_other.stack_initialized_ = false;
        t_other.network_started_ = false;
    }
    return *this;
}

void Server::destroy() noexcept {
    requestShutdown();
    if (network_started_) {
        NetworkHandlerFinish();
        network_started_ = false;
    }
    if (stack_initialized_) {
        ShutdownCipStack();
        stack_initialized_ = false;
    }
    vendor_class_ = nullptr;
    instanceTable().clear();
}

sgrn::Result<Server> Server::create(ServerConfig t_cfg) {
    return Server(std::move(t_cfg));
}

sgrn::Result<void> Server::initializeStack(uint8_t t_unique_connection_id) {
    if (stack_initialized_)
        return {};

    if (CipStackInit(t_unique_connection_id) != kEipStatusOk)
        return "CipStackInit failed";

    g_tcpip.interface_configuration.ip_address = ::inet_addr(cfg_.ip.c_str());
    g_tcpip.interface_configuration.network_mask = ::inet_addr("255.255.255.0");
    g_tcpip.config_control = kTcpipCfgCtrlStaticIp;

    stack_initialized_ = true;
    return {};
}

sgrn::Result<void> Server::createVendorClass(uint32_t t_class_code, uint32_t t_max_instance_attributes, std::string_view t_name) {
    if (!stack_initialized_)
        return "createVendorClass: stack not initialized";

    const uint32_t max_attrs = std::max(1u, t_max_instance_attributes);
    std::string name_str(t_name);
    vendor_class_ = CreateCipClass(t_class_code, 0, 0, 0, max_attrs, max_attrs, 2, 0, name_str.c_str(), 1, nullptr);
    if (!vendor_class_)
        return fmt::format("CreateCipClass(0x{:X}, '{}') failed", t_class_code, t_name);

    InsertGetSetCallback(vendor_class_, wrapperPreGetCallback, static_cast<CIPAttributeFlag>(kPreGetFunc));
    InsertGetSetCallback(vendor_class_, wrapperPostSetCallback, static_cast<CIPAttributeFlag>(kPostSetFunc));
    InsertService(vendor_class_, kGetAttributeSingle, &GetAttributeSingle, const_cast<char*>("GetAttributeSingle"));
    InsertService(vendor_class_, kSetAttributeSingle, &SetAttributeSingle, const_cast<char*>("SetAttributeSingle"));
    return {};
}

void Server::setGetSetCallbacks(PreGetCallback t_pre_get, PostSetCallback t_post_set) {
    pre_get_ = std::move(t_pre_get);
    post_set_ = std::move(t_post_set);
    g_pre_get = pre_get_;
    g_post_set = post_set_;
}

void Server::setByteArrayDecoder(CipAttributeDecodeFromMessage t_decoder) {
    byte_array_decoder_ = t_decoder;
    g_byte_array_decoder = t_decoder;
}

sgrn::Result<uint16_t> Server::addInstance(uint16_t t_instance_id) {
    if (!vendor_class_)
        return "addInstance: vendor class not created";

    CipInstance* p_inst = AddCipInstance(vendor_class_, t_instance_id);
    if (!p_inst)
        return fmt::format("AddCipInstance({} failed", t_instance_id);

    const uint16_t num = static_cast<uint16_t>(p_inst->instance_number);
    instanceTable()[num] = p_inst;
    return num;
}

sgrn::Result<void> Server::insertAttribute(uint16_t t_instance_number, const AttributeSpec& t_spec) {
    auto it = instanceTable().find(t_instance_number);
    if (it == instanceTable().end())
        return fmt::format("insertAttribute: unknown instance {}", t_instance_number);

    InsertAttribute(it->second, t_spec.attribute_number, t_spec.cip_type, t_spec.encode, t_spec.decode, t_spec.data, t_spec.flags);
    return {};
}

sgrn::Result<void> Server::startNetwork() {
    if (!stack_initialized_)
        return "startNetwork: stack not initialized";
    if (network_started_)
        return {};

    if (NetworkHandlerInitialize() != kEipStatusOk)
        return "NetworkHandlerInitialize failed";

    network_started_ = true;
    return {};
}

sgrn::Result<void> Server::processCyclic(uint32_t t_connection_timeout_ms) {
    if (!network_started_)
        return "processCyclic: network not started";
    if (shutdown_requested_.load(std::memory_order_acquire))
        return {};

    if (NetworkHandlerProcessCyclic() != kEipStatusOk)
        return "NetworkHandlerProcessCyclic failed";

    ManageConnections(t_connection_timeout_ms);
    return {};
}

void Server::requestShutdown() noexcept {
    shutdown_requested_.store(true, std::memory_order_release);
}

bool Server::shutdownRequested() const noexcept {
    return shutdown_requested_.load(std::memory_order_acquire);
}

void Server::shutdown() noexcept {
    destroy();
}

} // namespace sgrn::gateway::wrappers::ethernetip

// OpENer required application hooks — co-located with Server.cpp so the linker
// pulls them whenever the adapter references wrappers::ethernetip::Server.
extern "C" {

EipStatus ApplicationInitialization(void) {
    return kEipStatusOk;
}

void HandleApplication(void) {
}

void CheckIoConnectionEvent(unsigned int, unsigned int, IoConnectionEvent) {
}

EipStatus AfterAssemblyDataReceived(CipInstance* tp_instance) {
    (void)tp_instance;
    return kEipStatusOk;
}

EipBool8 BeforeAssemblyDataSend(CipInstance* tp_instance) {
    (void)tp_instance;
    return true;
}

EipStatus ResetDevice(void) {
    CloseAllConnections();
    CipQosUpdateUsedSetQosValues();
    return kEipStatusOk;
}

EipStatus ResetDeviceToInitialConfiguration(void) {
    g_tcpip.encapsulation_inactivity_timeout = 120;
    CipQosResetAttributesToDefaultValues();
    return ResetDevice();
}

void* CipCalloc(size_t t_number_of_elements, size_t t_size_of_element) {
    return std::calloc(t_number_of_elements, t_size_of_element);
}

void CipFree(void* tp_data) {
    std::free(tp_data);
}

void RunIdleChanged(EipUint32 t_run_idle_value) {
    CipIdentitySetExtendedDeviceStatus(
        (0x0001 & t_run_idle_value) == 1 ? kAtLeastOneIoConnectionInRunMode : kAtLeastOneIoConnectionEstablishedAllInIdleMode);
}

} // extern "C"
