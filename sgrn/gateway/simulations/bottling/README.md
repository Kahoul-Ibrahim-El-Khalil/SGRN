# Bottling & Packaging Plant Simulation (Full Plant-Scale)

## Overview
This scenario simulates a plant-scale beverage bottling operation with **two
parallel filling lines** feeding a **shared palletizing cell**, a common
**product supply / CIP skid**, and plant-wide **utilities** and **safety**
systems.

- **Line 1** — 750 mL glass bottles, still product, rated 300 bpm.
  Infeed → Rinser → Filler (40-valve rotary) → Capper (12-head) →
  Labeler (front/back) → Case Packer (12-pack).
- **Line 2** — 330 mL PET/can, carbonated product, rated 500 bpm.
  Infeed → Filler (24-valve rotary) → Capper (8-head) → Labeler →
  Case Packer (24-pack).
- Both lines merge onto a shared conveyor into one **robotic palletizer +
  stretch wrapper** (10 cases/layer, 6 layers/pallet).
- A single **20,000 L product tank** with agitator and CIP skid supplies
  both lines and cannot run product and CIP concurrently.
- **Utilities**: dual compressed-air compressors (6.8 bar header), a glycol
  chiller loop (2 °C supply, product cooling), and a plant vacuum header.
- **Safety**: 8 E-stop zones, 8 guard doors, 4 light curtains around the
  palletizer robot cell — any trip drops the whole plant to E-Stop.

It runs as a **single runtime process**, not a dual-client architecture:
`simulation.as` is an s7shell VM that holds the live DB memory, pushes it
to the Gateway over an S7 connection, and simultaneously exposes that same
memory as an embedded OPC-UA server on port 4840. There is no separate
HMI/operator script. Setpoints (`master_speed_sp`, `plant_mode`,
`reset_request`, `cip.active`, `estop_zones[i]`, etc.) are written directly
into the runtime by any external OPC-UA client — UaExpert, python-opcua, a
real HMI panel — connecting to `opc.tcp://<host>:4840`. The Gateway itself
never originates control; it's a passive downstream receiver that persists
and broadcasts whatever `simulation.as` pushes to it.

## What is Simulated?
- **Cascade speed control**: a single operator "master speed %" resolves
  into a per-line BPM target, which is then trimmed station-by-station using
  **inter-station accumulation buffers** — exactly how a real packaging line
  paces itself. A station feeding a buffer slows down as that buffer nears
  full; a station drawing from a buffer slows down as it runs dry. This
  produces realistic ripple effects: e.g. slow the capper and you'll see the
  filler-to-capper buffer fill up, throttling the filler back, which in turn
  drains the rinser buffer.
- **Shared resource contention**: both lines draw from one product tank; if
  the tank runs low, both fillers' bowl-level PID loops sag simultaneously.
  Triggering CIP forces plant speed to zero for the wash duration.
- **Statistical process variation**: fill volume, cap torque, and OEE are
  reported with realistic deterministic jitter around their setpoints, the
  way SPC charts would show natural variation around a target.
- **Palletizing**: case count accumulates through layers → pallets; a
  completed pallet triggers the stretch wrapper before releasing to the
  full-pallet conveyor.
- **Plant-wide safety**: any of the 8 E-stop zones, 8 guard doors, or 4
  light curtains trips a latched plant-wide E-Stop that forces every motor
  to zero speed and requires an explicit operator reset.
- **Utilities dynamics**: compressed air header sags under demand and
  recovers as a lag-compressor kicks in; glycol supply temperature is PID
  controlled against ambient heat-in.
- **Priority-ordered publishing**: the runtime's dirty-range tracking makes
  `.put()` cheap and incremental — only fields that actually changed since
  the last call go out over S7. `SafetySystems`, `Supervisor`'s mode/estop,
  and `Alarms` are flushed the instant they're computed each scan, so an
  E-stop or a new alarm reaches the Gateway ahead of that scan's routine
  process telemetry, which is flushed once at the end of the cycle.

## Enabled / Disabled Features
- **S7 Adapter**: **Enabled** (Port 102). `simulation.as` connects here to push the runtime's state to the gateway.
- **HTTP/WebSocket API**: **Enabled** (Ports 8000/8001).
- **Modbus Adapter**: **Disabled** — commented out in `security.as`.
- **EtherNet/IP**: **Disabled** — no rotating equipment on this network segment.
- **OPC-UA**: **Enabled** (Port 4840, hosted inside `simulation.as`) — this is the setpoint/control interface; connect any OPC-UA client here to start/stop the plant, change speed, trigger CIP, or trip E-stops.
- **Security**: **Strict Mode**. Policy enforced via `security.as`.
- **Persistence**: **Enabled**, all 18 DBs.

## Naming Convention
No field name encodes a unit (no `_ml`, `_bar`, `_nm`, `_pct` suffixes) —
every physical quantity carries a `#UNIT(...)` annotation in `schema.scl`
instead. Where three or more sibling fields shared a common prefix (e.g.
`torque_sp` / `torque_avg` / `torque_stddev`), they're grouped into a
nested `STRUCT` so the prefix is paid once, accessed as e.g.
`line1_capper.torque.sp`. The same pattern collapses `line1_*` / `line2_*`
fields on `Supervisor`, `tank_*` / `cip_*` on `ProductSupply`, `air_*` /
`glycol_*` / `vacuum_*` on `Utilities`, and `robot_*` / `wrapper_*` on
`Palletizer`.

## Data Blocks
| DB | Name              | Purpose                                             |
|----|-------------------|------------------------------------------------------|
| 1  | Supervisor        | Plant/line mode, master speed cascade, OEE, counts   |
| 2  | ProductSupply     | Shared batch tank + CIP skid                         |
| 3  | Line1_Infeed      | Unscrambler + accumulation table, Line 1              |
| 4  | Line1_Rinser      | Air/water bottle rinser, Line 1                       |
| 5  | Line1_Filler      | 40-valve rotary filler, Line 1                        |
| 6  | Line1_Capper      | 12-head chuck capper, Line 1                          |
| 7  | Line1_Labeler     | Front/back labeler, Line 1                            |
| 8  | Line1_CasePacker  | Case erector/packer + glue, Line 1                    |
| 9  | Line2_Infeed      | Unscrambler + accumulation table, Line 2              |
| 10 | Line2_Filler      | 24-valve rotary filler, Line 2                        |
| 11 | Line2_Capper      | 8-head chuck capper, Line 2                           |
| 12 | Line2_Labeler     | Front/back labeler, Line 2                            |
| 13 | Line2_CasePacker  | Case erector/packer + glue, Line 2                    |
| 14 | ConveyorNetwork   | Inter-station buffers + merge conveyor                |
| 15 | Palletizer        | Shared robotic palletizer + stretch wrapper           |
| 16 | Utilities         | Compressed air, glycol chiller, plant vacuum          |
| 17 | SafetySystems     | E-stop zones, guard doors, light curtains             |
| 18 | Alarms            | Plant-wide aggregated alarm summary                   |

## Setting Setpoints via OPC-UA
Since there's no operator script, drive the plant by writing nodes directly
on `opc.tcp://<host>:4840` (hosted inside `simulation.as`). Any OPC-UA
client works — e.g. UaExpert for interactive use, or
[python-opcua](https://github.com/FreeOpcUa/python-opcua) for scripting:

```python
from opcua import Client

client = Client("opc.tcp://localhost:4840")
client.connect()

def node(path):
    return client.get_root_node().get_child(path)

# Start the plant at 70% master speed
node(["0:Objects", "0:Supervisor", "0:reset_request"]).set_value(True)
node(["0:Objects", "0:Supervisor", "0:plant_mode"]).set_value(1)      # Starting
node(["0:Objects", "0:Supervisor", "0:master_speed_sp"]).set_value(70.0)

# Trigger a CIP wash
node(["0:Objects", "0:ProductSupply", "0:cip", "0:active"]).set_value(True)

# Trip an E-stop zone (index 0 = Infeed1)
node(["0:Objects", "0:SafetySystems", "0:estop_zones"]).set_value([True] + [False]*7)

client.disconnect()
```

> Exact browse-path segments depend on how the gateway's schema loader
> exposes the address space — browse from the OPC-UA client first to
> confirm node names if the paths above don't resolve directly.

## How to Test
1. Run the gateway with `gateway.json`, then start `simulation.as` (via
   `s7shell simulation.as`) — this single process is both the physics
   runtime and the OPC-UA server.
2. Connect an OPC-UA client to `opc.tcp://localhost:4840` and write
   `Supervisor.master_speed_sp` (e.g. 70) plus `Supervisor.plant_mode = 1`
   to start the plant.
3. Watch `DB14` (ConveyorNetwork) buffer `line1.*.fill` / `line2.*.fill`
   values ripple as you change speed — that's the cascade control in action.
4. Write `ProductSupply.cip.active = true` and watch both lines' speed
   setpoints drop to zero and `DB2.cip.cycle_state` step through PreRinse →
   Caustic → IntRinse → Acid → FinalRinse → Drain.
5. Write `SafetySystems.estop_zones[i] = true` for any zone, or
   `SafetySystems.light_curtain_broken[i] = true`, and watch
   `DB1.plant_estop` latch **immediately** (it's pushed the instant it's
   computed, not queued behind the rest of that scan) while every motor in
   `DB3`-`DB15` ramps to zero on the following scans. Clear all zones and
   write `Supervisor.reset_request = true` to recover.

> [!TIP]
> **Forcing Values:** Because the Gateway acts as a unified hub, you can
> also interactively force variables and observe the running physics by
> connecting a second OPC-UA client, or
> by directly connecting with the **`s7shell`** in a separate terminal.
