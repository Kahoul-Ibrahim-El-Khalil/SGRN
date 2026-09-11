# Pump & Tank Skid Simulation (MiniPlant)

## Overview
This scenario simulates a small, simplified process skid consisting of a feed pump, an inlet valve, a water tank with a heating element, and a gravity drain outlet valve. 

It demonstrates a **dual-client architecture**:
1. `plc_logic.as`: Acts as a Soft-PLC. It connects to the Gateway as an S7 client, reads the operator setpoints, runs a high-frequency (5 Hz) thermodynamic/hydraulic physics loop, and writes the state back to the Gateway.
2. `operator.as`: Acts as a SCADA/HMI script. It periodically connects to the Gateway, reads the system state, and writes automated setpoints (e.g., turning on the pump, changing heater setpoints, or triggering an Emergency Stop).

## What is Simulated?
- **Tank Hydraulics**: Non-linear gravity draining logic simulating Torricelli's Law (drain rate scales with hydrostatic head).
- **Pump Dynamics**: Ramping pump speeds, and **Cavitation faults** (if the pump is run at high speeds while the tank level drops below 5%, the pump cavitates, trips, and latches a fault).
- **Thermodynamics (PID)**: The PLC runs a PID loop regulating the heater output to match the operator's temperature setpoint, accounting for heat loss to the ambient environment.
- **Valve Actuation**: Valves don't jump to their setpoints instantly; they stroke over a defined `travel_time_s`.
- **Interlocks**: A hardwired overfill interlock trips the pump and forces the drain valve open if the tank level exceeds 99%.

## Enabled / Disabled Features
- **S7 Adapter**: **Enabled** (Port 102). *Both AngelScript clients connect via this port.*
- **HTTP/WebSocket API**: **Enabled** (Ports 8000/8001).
- **Modbus Adapter**: **Disabled** via `demo.py` due to root binding constraints.
- **Security**: **Strict Mode**. AngelScript policy enforced via `security.as`.
- **Persistence**: **Enabled**.

## How to Test
1. Connect via the Web Dashboard and observe `DB2` (Tank) and `DB5` (Heater).
2. The `operator.as` script is already running in the background via `demo.py`, automatically manipulating `DB1` (Setpoints). You will see the system dynamically responding, heating up, and cycling.
3. To trigger a cavitation fault, manually force the tank level low while the pump is running, or override the operator script's commands via the Web UI.

> [!TIP]
> **Forcing Values:** Because the Gateway acts as a unified hub, you can interactively force variables and override the running physics using an external **OPC-UA Client** (connecting to `opc.tcp://localhost:4840`) or by directly connecting with the **`s7shell`** in a separate terminal.
