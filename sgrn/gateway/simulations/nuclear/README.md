# Nuclear Power Plant Simulation (PWR)

## Overview
This scenario simulates a Pressurizer Water Reactor (PWR). It leverages the SGRN Gateway's ability to run a dynamic physics model directly in the edge device alongside the protocol translators.

## What is Simulated?
- **Reactor Kinetics**: Simulates neutron flux, reactivity, reactor period, and thermal power. Uses a simplified point-kinetics model balancing control rod position and boron concentration.
- **Xenon-135 Poisoning**: Xenon builds up as a fission byproduct when power is high, acting as a neutron poison and reducing reactivity. It decays away when power drops.
- **Decay Heat**: Following a SCRAM (reactor trip), fission power drops instantly, but decay heat persists (starting at ~6.5% of full power) and decays exponentially over time, requiring continued cooling.
- **Thermal-Hydraulics**: Primary loop temperatures (Core Inlet/Outlet) are derived dynamically from reactor power and Reactor Coolant Pump (RCP) flow rates.
- **RCP Coast-Down**: If power is lost or a SCRAM is initiated, the primary coolant pumps do not stop instantly; their momentum causes a coast-down, providing diminishing but crucial coolant flow.
- **Secondary Loop & Turbine**: Basic heat transfer to the Steam Generators, and electrical generation proportional to thermal power when synchronized to the grid.

## Enabled / Disabled Features
- **S7 Adapter**: **Enabled** (Port 102).
- **HTTP/WebSocket API**: **Enabled** (Ports 8000/8001).
- **Modbus Adapter**: **Disabled** via `demo.py` due to root binding constraints.
- **Security**: **Strict Mode**. AngelScript policy enforced via `security.as`.
- **Persistence**: **Enabled**.

## How to Test
1. Connect via the Web Dashboard and observe `DB1` (`ReactorCore`). 
2. Adjust `rod_bank_demand` to withdraw the control rods and observe the power ascension and subsequent Xenon buildup.
3. Trigger a SCRAM (e.g., by forcing `scram_manual = true` or dropping Steam Generator levels) and watch the RCP pumps coast down in `DB2` while the decay heat slowly drops.

> [!TIP]
> **Forcing Values:** Because the Gateway acts as a unified hub, you can interactively force variables and override the running physics using an external **OPC-UA Client** (connecting to `opc.tcp://localhost:4840`) or by directly connecting with the **`s7shell`** in a separate terminal.
