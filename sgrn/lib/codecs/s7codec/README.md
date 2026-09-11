# s7codec — Header-Only S7 Type Library

[![License: LGPL v3](https://img.shields.io/badge/License-LGPLv3-blue.svg)](https://www.gnu.org/licenses/lgpl-3.0)

s7codec is intended as a companion library to Snap7. 

It handles serialization and deserialization of Siemens S7 data types (e.g., DATE_AND_TIME, STRING, arrays of UDINT). 

You still need Snap7 (or another transport) to handle connection setup and PDU transfer.

A freestanding, dependency-free C++17 header-only library that defines the complete S7 type system (27+ types) and provides portable, bidirectional 
encode/decode routines for all industrial scalar, composite, and temporal types.

Note: This library simply decodes and encodes scalar values, and it is used in my own proprietary semantic engine that is under development.

## Why this exists?

In modern industrial software (IIoT, SCADA, Edge Computing), a major bottleneck is the S7 Semantic Opacity. Raw PLC memory is big-endian and follows 
specific alignment rules that are opaque to standard IT systems.

This library bridges that gap by providing:
1.  Hardware Agnosticism: The same C++ code runs on high-performance AArch64/x86 gateways and resource-constrained ESP32/Arduino field devices.
2.  Semantic Fidelity: Full support for complex types like DTL (TIA Portal Date and Time Long), BCD, and S7 String.
3.  Deployment Optimization: By using these headers, developers can define "Logical Shadows" of PLC Data Blocks as packed C++ structs, automatically handling big-endian conversion and alignment.

## Applications

I have developped the library as a submodule in my own thesis projects, then decided to make it standalone:

- **S7 Semantic Shell:** Built for scripting complex PLC simulations and automated testing directly against TIA Portal over S7.
- **Industrial Gateways:** Serving as the core codec for bridging S7 PLC memory to high-level protocols like **OPC UA** and **REST APIs**, 
    enabling  IT/OT integration.
- **Embedded S7 Clients:** Running on resource-constrained hardware like the **ESP32** to create low-cost, type-aware S7 field devices (using s7 as 
    transport protocol).

## Features

- Header-Only: Zero dependencies, easy integration.
- C++17 Freestanding: No STL requirements for core codec (optional std::string support).
- Comprehensive Type Support: Supports Bool, Int, Real, DInt, Time, Date, DTL, DateTime, String, WString, etc.
- Endianness Abstraction: BigEndian<T> templates for transparent byte-swapping.
- Tested on: Clang (Linux), Xtensa GCC (ESP32).

## Use Case Example

The most effective use of this library is creating high-level structural mirrors of PLC Data Blocks. By using the provided S7 types, 
you can map raw memory directly to a C++ structure that handles byte-swapping and bit-masking internally.

```cpp
#include <s7codec/s7.hpp>
#include <iostream>

// Mirroring a TIA Portal Data Block: "MotorControl" [DB10]
#pragma pack(push, 1)
struct MotorControlDB {
    s7codec::Bool  isRunning;      // Offset 0.0
    s7codec::Bool  hasError;       // Offset 0.1
    s7codec::Int   setpointRPM;    // Offset 2.0 (Big-Endian)
    s7codec::Real  currentTemp;    // Offset 4.0 (Big-Endian)
    s7codec::DTL   lastService;    // Offset 8.0 (12-byte Siemens DTL)
};
#pragma pack(pop)

void handlePlcData(uint8_t* buffer) {
    // Map the raw buffer to our structure
    auto* motor = reinterpret_cast<MotorControlDB*>(buffer);

    // Access values directly - conversion is handled by the types
    if (motor->isRunning) {
        std::cout << "Motor Speed: " << motor->setpointRPM << " RPM" << std::endl;
        std::cout << "Temperature: " << motor->currentTemp << " C" << std::endl;
    }
    
    // Modification: Update setpoint in the buffer
    motor->setpointRPM = 1500; 
}
```

## Handling UDTs (User-Defined Types)

There is no need for a specialized "UDT" class. In Siemens S7, a UDT is simply a memory layout template. 
In C++, this is perfectly represented by **composite structs**. 
By nesting structs composed of `s7codec` types, you maintain 1:1 binary compatibility with TIA Portal UDTs while keeping the code zero-overhead and 
type-safe.

```cpp
struct MotorUDT {
    s7codec::Int  rpm;
    s7codec::Real temp;
};

#pragma pack(push, 1)
struct MyLargeDB {
    MotorUDT motor1; // Equivalent to using UDT "MotorUDT" in TIA
    MotorUDT motor2;
};
#pragma pack(pop)
```

## Integration with Snap7

Because these structs are bit-compatible with PLC memory, you can simply cast them to `void*` or `uint8_t*` and pass them directly to Snap7's 
`DBWrite` or `DBRead`.

> **Important Note:** When using this with a physical S7-1200/1500 PLC, ensure that:
> 1. The Data Block has **"Optimized block access" turned OFF** (Standard access).
> 2. In the PLC Protection & Security settings, **"Permit access with PUT/GET communication from remote partner" is enabled**.
>
> These are mandatory requirements for any driver (like Snap7) that relies on absolute memory addressing.

```cpp
#include "snap7.h" // Snap7 C++ Client

TS7Client *Client = new TS7Client();
MotorControlDB data;

// Prepare data locally
data.isRunning = true;
data.setpointRPM = 3000;
data.lastService = s7codec::RawDTL::now();

// Send the entire structure to DB10, starting at offset 0
int res = Client->DBWrite(10, 0, sizeof(MotorControlDB), &data);

if (res == 0) {
    std::cout << "Successfully updated PLC memory!" << std::endl;
}
```

## Testing

A standalone example test is provided in `example_test.cpp`. To run it:

```bash
g++ -Iinclude example_test.cpp -o s7_test
./s7_test
```

## Usage

### Scalar Decoding
```cpp
#include <s7codec/s7.hpp>

// Decode a 16-bit signed integer from a big-endian PLC buffer
uint8_t buffer[] = {0x00, 0x2A};
auto val = s7codec::decodeScalar(s7codec::Type::Int, buffer);
// val.i == 42
```

## License

This library is licensed under the GNU Lesser General Public License v3.0 (LGPLv3). Contributions are highly appreciated!

---
*Developed by Kahoul Ibrahim El-Khalil.*
