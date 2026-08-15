
# SGRN — Component Utility & Interactivity Guide

**Purpose of this document:** define, unambiguously, what each SGRN component *is for*, whether it behaves **passively** (reacts to external stimuli, never initiates) or **actively** (can originate actions on its own schedule/logic), and how components talk to each other. This is the reference to point new contributors at before they touch code — a lot of design mistakes in a hub-and-spoke system come from a component overstepping its role (e.g. the gateway trying to "decide" something instead of just translating).

---

## 1. The one-paragraph mental model

The **gateway**  is a passive, hub-and-spoke **data aggregator and translator** — it mirrors PLC memory into a canonical model and re-exposes it through multiple protocols, 
but never decides anything on its own. **s7shell** is the active counterpart — it's where a human or a script actually *does* things: forcing values, running sequences, simulating a
soft PLC. **datastore** is the durable, large-scale storage substrate underneath everything — documents, telemetry dumps, historian archives — self-hosted, replacing the need for a third-party SaaS. **scl** sits at the boundary between "what the real PLC program looks like" and "what the gateway's typed model knows about" — it generates code from SCL schemas so the canonical model stays truthful to the actual PLC without hand-written bindings.

---

## 2. Component roles at a glance

| Component | Passive / Active | Primary job | Initiates connections? | Can write to a PLC on its own? |
|---|---|---|---|---|
| **gateway** (core) | **Passive** | Mirror PLC memory ↔ canonical `Registry`, translate to/from protocols | No (only outbound uplink to a parent, if federated) | No — only in response to an external client write/Method call |
| **s7shell** | **Active** | Interactive engineering, soft-PLC simulation, scripted sequences | Yes — connects out to a gateway or directly to a PLC | Yes — this is its whole purpose |
| **datastore** | Passive (storage) / reactive (API) | Durable object + document storage, telemetry archival, multi-tenant identity | No | N/A (not PLC-facing) |
| **scl** | Passive (build/config-time tool) | Parse SCL schemas → generate typed bindings/codec definitions | No (offline tool) | N/A |
| **codecs** (`s7codec`) | Passive (library) | Low-level type encode/decode, used by everything above it | N/A | N/A |
| **scripting** (AngelScript engine) | Shared runtime | Executes user/operator logic — used *actively* by s7shell, used *reactively* by gateway (only when invoked via an OPC UA Method call) | N/A | N/A directly — only via whatever it's embedded in |
| **sdk** | Passive (client library) | Thin client bindings for consuming gateway/datastore APIs from other applications | Yes, on behalf of whatever embeds it | Only if the embedding app tells it to |

---

## 3. Component detail

### 3.1 Gateway (core) — passive aggregator/translator

The gateway's entire behavior is: recieve data from PLC nodes → mirror it into the canonical `Registry` → forward deltas via `TelemetryBroker` → let each protocol adapter (OPC UA, MQTT, WebSocket, HTTP, EtherNet-IP) expose that canonical state in its own native shape.** 

Nothing about this loop originates a decision — it's a mirror, not an agent.

The **only** ways anything ever changes on the PLC side of the gateway are:
1. An external client issues an explicit write through one of the protocol adapters (OPC UA `Write`, Modbus write request, EtherNet/IP CIP write, WebSocket write message, HTTP `POST`/`PUT`, S7 PUT).
2. An external client invokes an OPC UA **Method** (e.g. triggering a scripted routine) — and even then, the gateway is just the transport; the logic runs in the shared scripting engine, not in gateway code.
3. In a federated topology, a **child gateway's** own mirrored state arrives via `DeltaEnvelope` and gets folded into the parent's `Registry` — this is still passive aggregation, just one level removed. The parent doesn't interpret or act on the data, it just re-namespaces and re-exposes it, exactly like local PLC data.
4. The gateway can recieve read events and serve the data itself, and it has an embedded WebUI which subscribes to field and serves data and visualizes it in a tabular form.

This is deliberate: keeping the gateway's core loop free of decision logic is what makes it safe to run unattended against live industrial hardware, and what makes the hub-and-spoke translation model (see the earlier architecture discussion) tractable — one code path handles reads, one handles writes, and every protocol adapter is a thin shape-conversion layer on top of that same path.

### 3.2 s7shell — active engineering tool

s7shell is where the "doing" happens. It is explicitly allowed to:
- **Simulate a soft PLC** — generate its own values, not just mirror a real one (useful for HIL testing SCADA/HMI systems without hardware).
- **Force I/O and run scripted sequences** via the shared AngelScript engine, on its own initiative — an operator or automated test script tells it to run something, and it runs.
- **Sync with a live gateway** (`GatewaySync`) — subscribe to the gateway's `TelemetryBroker` output over WebSocket to mirror real plant state into the shell's local view, and push writes back down through the gateway to the real PLC when the operator commands it.

The active/passive boundary is precisely: **s7shell decides to do something; the gateway only ever executes what it's told, by whoever is currently allowed to tell it.**

### 3.3 datastore — large-scale storage, no third-party SaaS required

datastore is the durable-storage and identity substrate. Framed correctly, it's not "the cloud backend" — it's a **self-hosted replacement** for the storage SaaS a team would otherwise pay a third party for:
- **General-purpose file/object storage** (S3-compatible semantics) — documents, calibration certs, maintenance SOPs, anything a plant needs to keep alongside its telemetry.
- **Telemetry dumps / historian archival** — the local per-gateway historian (Postgres/TimescaleDB, see the historian design) is the fast, local, operational store; datastore is where bulk exports/archival dumps land for long-term retention, cross-site aggregation, or reporting, at a scale a single gateway's local disk isn't meant for.
- **Multi-tenant identity & permissions** — `Users`/`Organisations`/`Sessions`, the layer that decides who can see which gateway's data, which document, which historical range.

The framing that matters: nothing about datastore is PLC-specific. It's generic large-scale storage infrastructure that happens to also serve the industrial side — which is exactly why it's a separate, independently useful component rather than something bolted onto the gateway.

### 3.4 scl — code generation from SCL schemas

SCL (Structured Control Language) is how Siemens PLC programs actually describe their own data structures. The `sclc` component's job is to **parse that source of truth and generate the typed bindings the rest of SGRN needs** — DB layouts, field types, offsets — so the gateway's canonical `Registry`/`Symbol` model stays accurate to what's actually running on the PLC, without a human hand-transcribing DB structures into C++.

This is a **build/config-time tool**, not a runtime component. Its output feeds:
- `codecs` (`s7codec`) — so the low-level encode/decode logic knows the real field layout.
- `core`'s `PlcSchemaStore` — so the gateway's address space (OPC UA nodes, REST paths, etc.) is generated from the real PLC program instead of manually configured.

### 3.5 codecs / scripting / sdk — shared infrastructure

- **`codecs` (`s7codec`)** is a freestanding, dependency-light library (runs on ESP32-class hardware) that does the actual byte-level S7 type encode/decode. Everything above it — gateway adapters, `scl`-generated bindings — calls into it; it calls into nothing.
- **`scripting`** (AngelScript engine) is genuinely shared: s7shell embeds it for active, operator-driven scripting; the gateway embeds the *same* engine, but only ever invokes it reactively, in response to an OPC UA Method call. Same runtime, opposite postures.
- **`sdk`** is a thin client library for external applications to talk to the gateway/datastore APIs without hand-rolling protocol clients. It's passive in the same sense the gateway is — it does exactly what the application embedding it tells it to, nothing more.
