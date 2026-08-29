#include <sgrn/debug.hpp>
#include <sgrn/gateway/adapters/ethernetip/CipCodec.hpp>
#include <sgrn/gateway/adapters/ethernetip/EipAdapter.hpp>
#include <sgrn/gateway/adapters/ethernetip/TypeTranslation.hpp>
#include <sgrn/gateway/adapters/ethernetip/errors.hpp>
#include <sgrn/gateway/common/SecurityHelper.hpp>
#include <sgrn/gateway/wrappers/ethernetip/Types.hpp>
#include <sgrn/scl/types.hpp>
#include <s7codec/endian.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace sgrn::gateway::adapters::ethernetip
{

using wrappers::ethernetip::attributeNumber;
using wrappers::ethernetip::AttributeSpec;
using wrappers::ethernetip::instanceNumber;
using wrappers::ethernetip::kS7GatewayClassCode;

static std::string getEipClientIp(void* /*instance*/, void* /*attribute*/) {
    return ""; // Placeholder: extract client IP from OpENer session if possible.
}

// Custom CIP ByteArray decoder for array attributes (pycomm3 compatibility).
extern "C" int eipDecodeArrayAttribute(
    void* const tp_data, CipMessageRouterRequest* const tp_request, CipMessageRouterResponse* const tp_response) {
    auto* p_ba = static_cast<CipByteArray*>(tp_data);
    const EipUint16 incoming = static_cast<EipUint16>(tp_request->request_data_size);
    const EipUint16 payload_len = (incoming == p_ba->length + 2) ? p_ba->length : incoming;

    if (payload_len == 0) {
        tp_response->general_status = kCipErrorNotEnoughData;
        return -1;
    }
    if (payload_len > p_ba->length) {
        SGRN_WARN_LOG("EtherNet/IP Decode: REJECT payload_len={} > capacity={}", payload_len, p_ba->length);
        tp_response->general_status = kCipErrorTooMuchData;
        return -1;
    }
    std::memcpy(p_ba->data, tp_request->data, payload_len);
    tp_response->general_status = kCipErrorSuccess;
    return static_cast<int>(payload_len);
}

EipAdapter::EipAdapter(twin::PlcMemory& t_memory, std::shared_ptr<::sgrn::gateway::SecurityManager> tsp_security_manager)
    : common::AdapterBase<EipAdapter>(t_memory, std::move(tsp_security_manager)) {
}

EipAdapter::~EipAdapter() {
    stop();
    for (auto& alloc : allocated_attrs_) {
        if (alloc->is_array)
            std::free(static_cast<CipByteArray*>(alloc->buffer)->data);
        std::free(alloc->buffer);
    }
    allocated_attrs_.clear();
}

sgrn::Result<void, ::sgrn::scl::SclError> EipAdapter::start(
    const std::string& t_ip, uint16_t t_port, const ::sgrn::scl::PlcSchemaStore& t_store) {
    // Store configuration for configure() to use
    config_ip_ = t_ip;
    config_port_ = t_port;
    p_store_ = &t_store;

    // Call AdapterBase::start which invokes configure() then serveLoop()
    auto res = common::AdapterBase<EipAdapter>::start(t_ip, t_port);
    if (res.hasError()) {
        return ::sgrn::scl::SclError::Generic;
    }
    return {};
}

bool EipAdapter::configure(const std::string& /*t_ip*/, uint16_t /*t_port*/) {
    if (!p_store_) {
        SGRN_ERROR_LOG("EtherNet/IP: PlcSchemaStore not set during configure");
        return false;
    }

    wrappers::ethernetip::ServerConfig cfg;
    cfg.ip = config_ip_;
    cfg.port = config_port_;

    auto server_res = wrappers::ethernetip::Server::create(std::move(cfg));
    if (server_res.hasError()) {
        SGRN_ERROR_LOG("EtherNet/IP: Server create failed: {}", server_res.error());
        return false;
    }

    server_ = std::move(server_res.value());

    if (auto r = server_->initializeStack(); r.hasError()) {
        server_.reset();
        SGRN_ERROR_LOG("EtherNet/IP: initializeStack failed: {}", r.error());
        return false;
    }

    server_->setByteArrayDecoder(eipDecodeArrayAttribute);
    server_->setGetSetCallbacks(
        [this](void* instance, void* attribute, uint8_t service) {
            return handlePreGet(instance, attribute, service) == 0 ? kEipStatusOk : kEipStatusError;
        },
        [this](void* instance, void* attribute, uint8_t service) {
            return handlePostSet(instance, attribute, service) == 0 ? kEipStatusOk : kEipStatusError;
        });

    const uint32_t max_fields = std::max(1u, [&] {
        uint32_t m = 0;
        for (const auto& [_, schema] : p_store_->dbs())
            m = std::max(m, static_cast<uint32_t>(schema.fields.size()));
        return m;
    }());

    if (auto r = server_->createVendorClass(kS7GatewayClassCode, max_fields, "S7Gateway"); r.hasError()) {
        server_->shutdown();
        server_.reset();
        SGRN_ERROR_LOG("EtherNet/IP: createVendorClass failed: {}", r.error());
        return false;
    }

    using sgrn::scl::DataType;
    struct CipBinding {
        uint8_t type;
        int size;
        CipAttributeEncodeInMessage enc;
        CipAttributeDecodeFromMessage dec;
    };
    static const std::pair<DataType, CipBinding> bindings[] = {
        {DataType::Bool, {kCipBool, 1, EncodeCipBool, (CipAttributeDecodeFromMessage)DecodeCipBool}},
        {DataType::Byte, {kCipUsint, 1, EncodeCipUsint, (CipAttributeDecodeFromMessage)DecodeCipUsint}},
        {DataType::USInt, {kCipUsint, 1, EncodeCipUsint, (CipAttributeDecodeFromMessage)DecodeCipUsint}},
        {DataType::Char, {kCipUsint, 1, EncodeCipUsint, (CipAttributeDecodeFromMessage)DecodeCipUsint}},
        {DataType::SInt, {kCipSint, 1, EncodeCipSint, (CipAttributeDecodeFromMessage)DecodeCipSint}},
        {DataType::Word, {kCipUint, 2, EncodeCipUint, (CipAttributeDecodeFromMessage)DecodeCipUint}},
        {DataType::UInt, {kCipUint, 2, EncodeCipUint, (CipAttributeDecodeFromMessage)DecodeCipUint}},
        {DataType::Int, {kCipInt, 2, EncodeCipInt, (CipAttributeDecodeFromMessage)DecodeCipInt}},
        {DataType::DWord, {kCipUdint, 4, EncodeCipUdint, (CipAttributeDecodeFromMessage)DecodeCipUdint}},
        {DataType::UDInt, {kCipUdint, 4, EncodeCipUdint, (CipAttributeDecodeFromMessage)DecodeCipUdint}},
        {DataType::Time, {kCipUdint, 4, EncodeCipUdint, (CipAttributeDecodeFromMessage)DecodeCipUdint}},
        {DataType::TimeOfDay, {kCipUdint, 4, EncodeCipUdint, (CipAttributeDecodeFromMessage)DecodeCipUdint}},
        {DataType::DInt, {kCipDint, 4, EncodeCipDint, (CipAttributeDecodeFromMessage)DecodeCipDint}},
        {DataType::Real, {kCipReal, 4, EncodeCipReal, (CipAttributeDecodeFromMessage)DecodeCipReal}},
        {DataType::LWord, {kCipUlint, 8, EncodeCipUlint, (CipAttributeDecodeFromMessage)DecodeCipUlint}},
        {DataType::ULInt, {kCipUlint, 8, EncodeCipUlint, (CipAttributeDecodeFromMessage)DecodeCipUlint}},
        {DataType::LTime, {kCipUlint, 8, EncodeCipUlint, (CipAttributeDecodeFromMessage)DecodeCipUlint}},
        {DataType::LTimeOfDay, {kCipUlint, 8, EncodeCipUlint, (CipAttributeDecodeFromMessage)DecodeCipUlint}},
        {DataType::LInt, {kCipLint, 8, EncodeCipLint, (CipAttributeDecodeFromMessage)DecodeCipLint}},
        {DataType::LReal, {kCipLreal, 8, EncodeCipLreal, (CipAttributeDecodeFromMessage)DecodeCipLreal}},
    };
    static const CipBinding kArrayFallback{
        kCipByteArray, int(sizeof(CipByteArray)), EncodeCipByteArray, (CipAttributeDecodeFromMessage)eipDecodeArrayAttribute};

    for (const auto& [db_number, schema] : p_store_->dbs()) {
        auto inst_res = server_->addInstance(db_number);
        if (inst_res.hasError())
            continue;

        const uint16_t cip_instance_num = inst_res.value();
        db_to_cip_instance_[db_number] = cip_instance_num;
        SGRN_INFO_LOG("EtherNet/IP: DB{} -> CIP Instance {}", db_number, cip_instance_num);

        uint16_t attr_num = 1;
        for (const auto& field : schema.fields) {
            auto alloc = std::make_shared<AllocatedAttribute>();
            alloc->db_number = db_number;
            alloc->field_path = field.name;
            alloc->type = field.type;
            alloc->field_offset = field.offset;
            alloc->bit_index = field.bit_index;
            alloc->endianness = field.endianness;
            alloc->field = field;

            const bool is_composite =
                field.count > 1 || !field.children.empty() || field.type == DataType::String || field.type == DataType::WString;

            const CipBinding* binding = &kArrayFallback;
            if (!is_composite) {
                auto it = std::find_if(std::begin(bindings), std::end(bindings), [&](const auto& p) { return p.first == field.type; });
                binding = (it != std::end(bindings)) ? &it->second : &kArrayFallback;
            }
            alloc->is_array = (binding->type == kCipByteArray);

            alloc->size = sgrn::scl::rawTypeSpanBytes(field.type, field.count);
            int alloc_size = binding->size;
            if (alloc->is_array) {
                const size_t cip_data_size = CipCodec::computeCipSize(field);
                alloc->size = cip_data_size;
                alloc_size = sizeof(CipByteArray);
                SGRN_INFO_LOG("EtherNet/IP INIT: DB{}/{} cip_data_size={}", db_number, field.name, cip_data_size);
            }

            alloc->buffer = std::calloc(1, alloc_size);
            if (alloc->is_array) {
                auto* p_ba = static_cast<CipByteArray*>(alloc->buffer);
                p_ba->length = static_cast<EipUint16>(alloc->size);
                p_ba->data = static_cast<EipByte*>(std::calloc(1, alloc->size));
            }

            allocated_attrs_.push_back(alloc);
            attr_map_[{cip_instance_num, attr_num}] = alloc;

            AttributeSpec spec;
            spec.attribute_number = attr_num;
            spec.cip_type = binding->type;
            spec.encode = binding->enc;
            spec.decode = binding->dec;
            spec.data = alloc->buffer;
            spec.flags = kSetAndGetAble | kPreGetFunc | kPostSetFunc;
            if (auto ins = server_->insertAttribute(cip_instance_num, spec); ins.hasError()) {
                SGRN_WARN_LOG("EtherNet/IP: insertAttribute DB{}/{} failed: {}", db_number, field.name, ins.error());
            }
            attr_num++;
        }
    }

    if (auto r = server_->startNetwork(); r.hasError()) {
        server_->shutdown();
        server_.reset();
        SGRN_ERROR_LOG("EtherNet/IP: startNetwork failed: {}", r.error());
        return false;
    }

    SGRN_INFO_LOG("EtherNet/IP Adapter configured on {}:{}", config_ip_, wrappers::ethernetip::kDefaultPort);
    return true;
}

void EipAdapter::stop() {
    if (server_)
        server_->requestShutdown();
    common::AdapterBase<EipAdapter>::stop();
    if (server_)
        server_->shutdown();
    server_.reset();
    attr_map_.clear();
    db_to_cip_instance_.clear();
}

void EipAdapter::serveLoop() {
    while (runningFlag().load() && server_ && !server_->shutdownRequested()) {
        if (auto r = server_->processCyclic(10); r.hasError()) {
            SGRN_ERROR_LOG("OpENer processCyclic failed: {}", r.error());
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int EipAdapter::handlePreGet(void* tp_instance_ptr, void* tp_attribute_ptr, uint8_t /*service*/) {
    auto it = attr_map_.find({instanceNumber(tp_instance_ptr), attributeNumber(tp_attribute_ptr)});
    if (it == attr_map_.end())
        return -1;
    auto& alloc = it->second;

    const size_t s7_size = TypeTranslation::s7SpanBytes(alloc->field);
    std::vector<uint8_t> raw_buf(s7_size, 0);
    if (s7_size == 0 || !(getMemory().readDbMemory(alloc->db_number, alloc->field_offset, raw_buf.size(), raw_buf.data()))) {
        SGRN_WARN_LOG("EtherNet/IP PreGet: readDbMemory failed for DB{}/{}", alloc->db_number, alloc->field_path);
        return -1;
    }

    if (alloc->is_array) {
        auto* p_ba = static_cast<CipByteArray*>(alloc->buffer);
        std::memset(p_ba->data, 0, alloc->size);
        auto f_copy = alloc->field;
        f_copy.offset = 0;
        size_t cip_offset = 0;
        CipCodec::encodeToCip(f_copy, raw_buf.data(), p_ba->data, cip_offset);
        return 0;
    }

    // Shared scalar decode — same DataType→size mapping for all adapters.
    TypeTranslation::loadScalarFromS7(alloc->field, raw_buf.data(), alloc->buffer);
    return 0;
}

int EipAdapter::handlePostSet(void* tp_instance_ptr, void* tp_attribute_ptr, uint8_t /*service*/) {
    if (auto sec_mgr = getSecurityManager()) {
        std::string client_ip = getEipClientIp(tp_instance_ptr, tp_attribute_ptr);
        if (!sec_mgr->authorizeEip(client_ip)) {
            SGRN_WARN_LOG("EtherNet/IP: Write denied for {}", client_ip.empty() ? "unknown IP" : client_ip);
            return -1;
        }
    }

    auto it = attr_map_.find({instanceNumber(tp_instance_ptr), attributeNumber(tp_attribute_ptr)});
    if (it == attr_map_.end())
        return -1;
    auto& alloc = it->second;

    using sgrn::scl::DataType;

    if (alloc->is_array) {
        auto* p_ba = static_cast<CipByteArray*>(alloc->buffer);
        const size_t s7_size = TypeTranslation::s7SpanBytes(alloc->field);
        if (s7_size == 0) {
            SGRN_WARN_LOG("EtherNet/IP PostSet: s7_size==0 for DB{}/{}, skipping write", alloc->db_number, alloc->field_path);
            return 0;
        }
        std::vector<uint8_t> raw_buf(s7_size, 0);
        if (!(getMemory().readDbMemory(alloc->db_number, alloc->field_offset, raw_buf.size(), raw_buf.data()))) {
            SGRN_WARN_LOG("EtherNet/IP PostSet: readDbMemory failed for DB{}/{}", alloc->db_number, alloc->field_path);
            return 0;
        }
        auto f_copy = alloc->field;
        f_copy.offset = 0;
        size_t cip_offset = 0;
        CipCodec::decodeFromCip(f_copy, p_ba->data, raw_buf.data(), cip_offset);
        if (auto r = getMemory().writeDbMemory(alloc->db_number, alloc->field_offset, s7_size, raw_buf.data()); !r) {
            SGRN_WARN_LOG("EtherNet/IP PostSet: writeDbMemory failed for DB{}/{}: {} (cip_status=0x{:02x})", alloc->db_number,
                alloc->field_path, r.error(), toCipStatus(r.error()));
        }
        return 0;
    }

    if (alloc->type == DataType::Bool) {
        const bool val = (*static_cast<uint8_t*>(alloc->buffer) != 0);
        if (auto r = getMemory().writeBit(alloc->db_number, alloc->field_offset, alloc->bit_index, val); !r) {
            SGRN_WARN_LOG("EtherNet/IP PostSet: writeBit failed for DB{}/{}: {} (cip_status=0x{:02x})", alloc->db_number, alloc->field_path,
                r.error(), toCipStatus(r.error()));
            return -1;
        }
        return 0;
    }

    // Shared scalar encode — same DataType→size mapping for all adapters.
    const size_t s7_size = sgrn::scl::rawTypeSpanBytes(alloc->type, 1);
    std::vector<uint8_t> raw_buf(s7_size, 0);
    TypeTranslation::storeScalarToS7(alloc->field, alloc->buffer, raw_buf.data());
    if (auto r = getMemory().writeDbMemory(alloc->db_number, alloc->field_offset, s7_size, raw_buf.data()); !r) {
        SGRN_WARN_LOG("EtherNet/IP: writeDbMemory DB{}/{} failed: {} (cip_status=0x{:02x})", alloc->db_number, alloc->field_path, r.error(),
            toCipStatus(r.error()));
        return -1;
    }
    return 0;
}

} // namespace sgrn::gateway::adapters::ethernetip
