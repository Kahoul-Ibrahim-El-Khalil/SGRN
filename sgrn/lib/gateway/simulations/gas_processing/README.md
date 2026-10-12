# Gas Processing Simulation (Molecular Sieve Dehydration)

## Overview
This scenario simulates a natural gas dehydration unit using a 3-tower molecular sieve design. It is highly elaborate, modelling dynamic physics, thermodynamic equations, and mechanical wear over time.

## What is Simulated?
- **3-Tower State Machine**: The 3 adsorber towers cycle through Adsorption, Depressurization, Heating, Cooling, and Repressurization states dynamically.
- **Process Noise**: Random noise is injected into feed pressures, feed flows, and blower flows to emulate real-world sensor fluctuations and process instability.
- **Fouling**: The inlet coalescer filter gradually fouls over time (differential pressure increases).
- **Thermodynamics (PID)**: The regeneration gas heater employs a PID-like response curve to ramp up to the 290°C target during the heating cycle and cool down naturally during the cooling cycle.
- **Moisture Saturation**: Desiccant beds gradually saturate with moisture depending on the incoming feed flow rate. The sales gas dew point directly reflects the condition of the worst-performing online tower.
- **Fault Scenarios**:
  - `fault_code = 8`: Molecular sieve non-absorption at Hour 4; active adsorber fails to retain moisture leading to rapid dew point breakthrough.
  - `fault_code = 9`: **FLARE_BLOCKED** at Hour 3 of the fault day (Day 31). Physically models an obstructed flare header line where relief is ineffective. `flare_header_press` ramps up from 1.05 bar at ~+0.15 bar/tick; `feed_press` climbs gradually (+1.15 bar/tick), causing `scrubber_press`, `system_inlet_press`, and `system_outlet_press` to rise with dynamic lag alongside small `bed_dp` increases.
- **Emergency Shutdown (ESD)**: If the dew point spec is grossly violated, feed pressure spikes (`feed_press > 90.0` bar), or the heater runs away, the ESD trips.

## Enabled / Disabled Features
- **S7 Adapter**: **Enabled** (Port 102).
- **HTTP/WebSocket API**: **Enabled** (Port 8000; WebSocket at `/ws`).
- **Modbus Adapter**: **Disabled** in the default simulation configuration due to port 502 root binding restrictions.
- **Security**: **Strict Mode**. AngelScript policy enforced via `security.as`.
- **Persistence**: **Enabled**. Snapshots are flushed to `/tmp/sgrn-gateway-state`.

## How to Test
1. Observe the cycling state of the towers in `DB2` (`AdsorberTowers`).
2. Watch the `coalescer_dp` in `DB1` slowly rise due to fouling.
3. Observe the `heater_outlet_temp` ramping up and down in `DB3` in sync with the tower states.

For the complete recorded-history experiment, run `python3 demos/history_twin.py` from the repository root. The isolated S7Shell simulation uses its `Persistence` binding to write the WAL, then the experiment replays that archive through `sgrn_replay` and runs a prediction gateway beside it. The temporary gateway serves the connection during capture but does not record the archive. The `--fault` option adds a selected fault (`8` = sieve non-absorption, `9` = flare blocked) to the isolated recording copy, leaving this baseline simulation unchanged.

For the minimal partial-loop prototype evaluating the flare blocked fault with PyTorch conformal anomaly detection and read-only LangChain agent monitoring, see [experiments/flare/README.md](../../../../experiments/flare/README.md).

> [!TIP]
> **Forcing Values:** Because the Gateway acts as a unified hub, you can interactively force variables and override the running physics using an external **OPC-UA Client** (connecting to `opc.tcp://localhost:4840`) or by directly connecting with the **`s7shell`** in a separate terminal.
