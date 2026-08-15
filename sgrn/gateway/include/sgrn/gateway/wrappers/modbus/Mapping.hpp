#pragma once

#include <sgrn/Result.hpp>

#include <cstdint>
#include <utility>

struct _modbus_mapping_t;
typedef struct _modbus_mapping_t modbus_mapping_t;

namespace sgrn::gateway::wrappers::modbus
{

/// RAII wrapper over libmodbus modbus_mapping_t.
class Mapping {
public:
    ~Mapping() noexcept;

    Mapping(Mapping&&) noexcept;
    Mapping& operator=(Mapping&&) noexcept;
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;

    static sgrn::Result<Mapping> create(int t_nb_bits, int t_nb_input_bits, int t_nb_registers, int t_nb_input_registers);

    modbus_mapping_t* raw() noexcept {
        return mapping_;
    }
    const modbus_mapping_t* raw() const noexcept {
        return mapping_;
    }

    int nbBits() const noexcept;
    int nbInputBits() const noexcept;
    int nbRegisters() const noexcept;
    int nbInputRegisters() const noexcept;

    uint8_t* bits() noexcept;
    uint8_t* inputBits() noexcept;
    uint16_t* registers() noexcept;
    uint16_t* inputRegisters() noexcept;

private:
    explicit Mapping(modbus_mapping_t* tp_mapping) noexcept;

    modbus_mapping_t* mapping_{nullptr};
};

} // namespace sgrn::gateway::wrappers::modbus
