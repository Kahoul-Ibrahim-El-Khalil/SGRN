# Gas Processing Simulation (Molecular Sieve Dehydration)

## Overview
This scenario simulates a natural gas dehydration unit using a 3-tower molecular sieve design. It is highly elaborate, modelling dynamic physics, thermodynamic equations, and mechanical wear over time.

## What is Simulated?
- **3-Tower State Machine**: The 3 adsorber towers cycle through Adsorption, Depressurization, Heating, Cooling, and Repressurization states dynamically.
- **Process Noise**: Random noise is injected into feed pressures, feed flows, and blower flows to emulate real-world sensor fluctuations and process instability.
- **Fouling**: The inlet coalescer filter gradually fouls over time (differential pressure increases).
- **Thermodynamics (PID)**: The regeneration gas heater employs a PID-like response curve to ramp up to the 290°C target during the heating cycle and cool down naturally during the cooling cycle.
- **Moisture Saturation**: Desiccant beds gradually saturate with moisture depending on the incoming feed flow rate. The sales gas dew point directly reflects the condition of the worst-performing online tower.
- **Emergency Shutdown (ESD)**: If the dew point spec is grossly violated, feed pressure spikes, or the heater runs away, the ESD trips.

## Enabled / Disabled Features
- **S7 Adapter**: **Enabled** (Port 102).
- **HTTP/WebSocket API**: **Enabled** (Ports 8000/8001).
- **Modbus Adapter**: **Disabled** in demo via `demo.py` due to port 502 root binding restrictions (unless run with `sudo`).
- **Security**: **Strict Mode**. AngelScript policy enforced via `security.as`.
- **Persistence**: **Enabled**. Snapshots are flushed to `/tmp/sgrn-gateway-state`.

## How to Test
1. Observe the cycling state of the towers in `DB2` (`AdsorberTowers`).
2. Watch the `coalescer_dp` in `DB1` slowly rise due to fouling.
3. Observe the `heater_outlet_temp` ramping up and down in `DB3` in sync with the tower states.

> [!TIP]
> **Forcing Values:** Because the Gateway acts as a unified hub, you can interactively force variables and override the running physics using an external **OPC-UA Client** (connecting to `opc.tcp://localhost:4840`) or by directly connecting with the **`s7shell`** in a separate terminal.
