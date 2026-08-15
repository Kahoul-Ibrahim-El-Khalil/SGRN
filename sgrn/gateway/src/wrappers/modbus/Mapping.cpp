#include <sgrn/gateway/wrappers/modbus/Mapping.hpp>

#include <modbus.h>

#include <fmt/core.h>

#include <utility>

namespace sgrn::gateway::wrappers::modbus
{

Mapping::Mapping(modbus_mapping_t* tp_mapping) noexcept
    : mapping_(tp_mapping) {
}

Mapping::~Mapping() noexcept {
    if (mapping_) {
        ::modbus_mapping_free(mapping_);
        mapping_ = nullptr;
    }
}

Mapping::Mapping(Mapping&& t_other) noexcept
    : mapping_(std::exchange(t_other.mapping_, nullptr)) {
}

Mapping& Mapping::operator=(Mapping&& t_other) noexcept {
    if (this != &t_other) {
        if (mapping_)
            ::modbus_mapping_free(mapping_);
        mapping_ = std::exchange(t_other.mapping_, nullptr);
    }
    return *this;
}

sgrn::Result<Mapping> Mapping::create(int t_nb_bits, int t_nb_input_bits, int t_nb_registers, int t_nb_input_registers) {
    modbus_mapping_t* p_mapping = ::modbus_mapping_new(t_nb_bits, t_nb_input_bits, t_nb_registers, t_nb_input_registers);
    if (!p_mapping)
        return fmt::format("modbus_mapping_new failed: {}", ::modbus_strerror(errno));
    return Mapping(p_mapping);
}

int Mapping::nbBits() const noexcept {
    return mapping_ ? mapping_->nb_bits : 0;
}

int Mapping::nbInputBits() const noexcept {
    return mapping_ ? mapping_->nb_input_bits : 0;
}

int Mapping::nbRegisters() const noexcept {
    return mapping_ ? mapping_->nb_registers : 0;
}

int Mapping::nbInputRegisters() const noexcept {
    return mapping_ ? mapping_->nb_input_registers : 0;
}

uint8_t* Mapping::bits() noexcept {
    return mapping_ ? mapping_->tab_bits : nullptr;
}

uint8_t* Mapping::inputBits() noexcept {
    return mapping_ ? mapping_->tab_input_bits : nullptr;
}

uint16_t* Mapping::registers() noexcept {
    return mapping_ ? mapping_->tab_registers : nullptr;
}

uint16_t* Mapping::inputRegisters() noexcept {
    return mapping_ ? mapping_->tab_input_registers : nullptr;
}

} // namespace sgrn::gateway::wrappers::modbus
