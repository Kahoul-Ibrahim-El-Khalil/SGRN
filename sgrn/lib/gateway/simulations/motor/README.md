# Motor / VFD Simulation

## Overview
A single induction motor driven by a simulated VFD (variable-frequency drive). Unlike the other
simulations, there's no separate "operator" script and no internal demand generator — the motor is
controlled *entirely* from the outside, over OPC-UA, exactly as if `MotorCommand` were the drive's
physical control terminals.

- `simulation.as` is the controlling unit itself (the VFD firmware): it reads `MotorCommand`,
  ramps the drive, models current draw, winding temperature, and protection trips, and publishes
  `MotorStatus` / `Alarms` back — all at 10 Hz.
- The embedded `OpcUaServer` in `simulation.as` exposes that same memory on `opc.tcp://<host>:4840`.
  There's no polling/round-trip on the command side: an OPC-UA write into `MotorCommand.start` (or
  any other command field) lands directly in the VM's memory and is read on the very next scan.

## What is Simulated?
- **Ramped start/stop**: speed moves from 0→100% (or wherever it's commanded) over `accel_time_s`,
  and back down over `decel_time_s` — not a step change.
- **Inrush current**: current spikes above steady-state while actively accelerating, decaying back
  to a load-proportional steady value once the target speed is reached.
- **Thermal model**: winding temperature rises with I², cools passively toward ambient, and trips
  the drive at 130°C.
- **Overcurrent protection**: a thermal-overload-relay-style trip — current has to stay above the
  limit for a couple of seconds before it actually trips, not an instant trip.
- **Stall protection**: commanded to run, drawing current, but speed stuck near zero for too long
  → stall trip (e.g. a locked rotor).
- **Torque-limit current foldback**: `torque_limit_pct` caps the current the "VFD" will deliver,
  the way a real drive's current limit does.
- **E-stop**: an immediate, hardwired-style fast stop (1 s coast-to-zero) independent of the
  configured decel ramp, and a latched fault.
- **Fault reset handshake**: `fault_reset` only clears a fault once the root cause (current, temp,
  stall condition) has actually cleared — you can't reset your way past an overtemp trip.

## Enabled / Disabled Features
- **S7 Adapter**: Enabled (port 102) — used internally by `simulation.as` to attach to its own
  schema; not the control channel.
- **OPC-UA**: **Enabled — this is the control channel.** `opc.tcp://localhost:4840`.
- **HTTP/WebSocket API**: Enabled (ports 8000/8001) for read-only dashboards.
- **Modbus / EtherNet/IP**: Disabled — single motor, no I/O network here.
- **Security**: Strict mode via `security.as`.
- **Persistence**: Enabled.

## How to Test
Connect any OPC-UA client to `opc.tcp://localhost:4840` and browse to `MotorCommand`.

A typical start-and-accelerate sequence:
1. Write `accel_time_s = 8.0`, `decel_time_s = 5.0`, `speed_sp_pct = 75.0`.
2. Write `start = true` (rising edge starts the ramp). Watch `MotorStatus.drive.speed_fb_pct`
   climb toward 75% over ~8 seconds, with `current_a` spiking above steady-state while it
   accelerates, then settling.
3. Write `speed_sp_pct = 100.0` while running — the drive re-ramps to the new target live.
4. Write `stop = true` — speed decays to 0 over `decel_time_s`.
5. To see a trip: write `speed_sp_pct = 100.0`, `accel_time_s = 0.5` (aggressive ramp) and
   `start = true` repeatedly — the resulting current spike will trip overcurrent. Clear it with
   `fault_reset = true` once `current_a` has settled back down.
6. Write `e_stop = true` at any time to see the hard 1-second stop and latched fault; release it
   and use `fault_reset` to clear.

> [!TIP]
> You can also watch it happen without any OPC-UA client at all by tailing the console output of
> `simulation.as` itself — it prints a status line once a second.
