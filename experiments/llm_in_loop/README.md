# LLM-in-the-loop Tank/Pump Skid Experiment

This experiment demonstrates an LLM-in-the-loop control system for a simulated tank/pump/heater skid using the SGRN framework.

## Architecture

```
┌─────────────────┐     WebSocket      ┌──────────────────┐
│  s7shell PLC    │ ◄─────────────────► │  Gateway (REST   │
│  (simulation.as)│   Binary Protocol  │   + WS + Hist)   │
└─────────────────┘                    └────────┬─────────┘
                                                │
                              HTTP REST         │
                         ┌──────────────────────┼──────────────────────┐
                         ▼                      ▼                      ▼
                  ┌─────────────┐        ┌─────────────┐        ┌─────────────┐
                  │  LLM Client │        │  Dashboard  │        │  Historian  │
                  │ (llm_client)│        │   (Web UI)  │        │  (Archives) │
                  └─────────────┘        └─────────────┘        └─────────────┘
```

## Components

- **schema.scl** - SCL schema defining 7 data blocks including `LLMAnalysis` with text fields for LLM analysis
- **simulation.as** - Soft PLC simulation running at 1 Hz with physics model
- **llm_client.py** - Python script that polls for LLM requests, calls LLM API, writes responses
- **gateway.json** - Gateway configuration
- **security.as** - Permissive security policy

## Data Blocks

| DB | Name | Description |
|----|------|-------------|
| 1 | Setpoints | Operator commands (pump, valves, heater, E-stop) |
| 2 | Tank | Process values (level, temp, flows, alarms) |
| 3 | Pump | Pump status (running, speed, fault) |
| 4 | Valves | Inlet/outlet valve actuator positions |
| 5 | Heater | PID temperature control loop |
| 6 | Alarms | Aggregated alarm summary |
| 7 | LLMAnalysis | LLM analysis text, suggested action, confidence |

## LLM Analysis Flow

1. Simulation (simulation.as) sets `LLMAnalysis.request_pending = true` every 10 seconds
2. Python client (llm_client.py) polls `LLMAnalysis` via REST API
3. When request detected, client reads all plant state DBs
4. Client builds prompt with current plant state
5. Client calls LLM API (OpenAI-compatible)
6. Client writes response to `LLMAnalysis.response` and clears `request_pending`
7. Simulation reads `LLMAnalysis.response.suggested_action` and applies it (if confidence > 70%)

## Quick Start

### 1. Build the Project

```bash
# From repository root
./scripts/build.sh linux-static-release
```

### 2. Configure LLM API

```bash
cd experiments/llm_in_loop
cp .env.example .env
# Edit .env with your API key and model preference
```

### 3. Start the Gateway

```bash
# From repository root
./.dist/linux-static-release/gateway experiments/llm_in_loop/gateway.json
```

### 4. Start the Simulation (in another terminal)

```bash
# From repository root
cd experiments/llm_in_loop
./.dist/linux-static-release/s7shell simulation.as
```

### 5. Start the LLM Client (in another terminal)

```bash
cd experiments/llm_in_loop
pip install websockets openai python-dotenv
python llm_client.py
```

### 6. View Dashboard

Open http://localhost:8000 in your browser.

## LLM Prompt Engineering

The prompt in `llm_client.py` includes:
- Full plant state (tank, pump, valves, heater, alarms)
- Current skid mode (OFF/FILLING/HEATING/DISCHARGING/ALARM)
- Previous LLM action for context
- Available actions with clear definitions
- Safety considerations

Modify `build_llm_prompt()` to adjust the prompt for your use case.

## Action Space

| Code | Action | Description |
|------|--------|-------------|
| 0 | HOLD | Maintain current state |
| 1 | INCREASE_FILL | Increase pump speed & inlet valve |
| 2 | DECREASE_FILL | Decrease pump speed & inlet valve |
| 3 | START_HEAT | Enable heater, setpoint 60°C |
| 4 | STOP_HEAT | Disable heater |
| 5 | OPEN_OUTLET | Increase outlet valve |
| 6 | CLOSE_OUTLET | Decrease outlet valve |
| 7 | EMERGENCY_STOP | Engage E-stop |

## Safety Features

- Simulation only applies LLM actions with confidence > 70%
- E-stop overrides all LLM actions
- High/low level alarms trigger ALARM mode
- Pump fault detection
- Overfill trip protection

## Extending

### Add More Sensors

1. Add fields to schema.scl data blocks
2. Update simulation.as physics model
3. Update llm_client.py `update_plant_state()` and `build_llm_prompt()`

### Change LLM Provider

The client uses OpenAI-compatible API. For other providers:
- Set `LLM_BASE_URL` to provider's endpoint
- Use appropriate `LLM_MODEL` name
- Ensure API key format matches provider

### Add Checker/Validator

Create a second Python script that:
1. Reads LLMAnalysis.response
2. Validates action against safety rules
3. Sets `action_blocked` flag if unsafe
4. Simulation respects `action_blocked`

## Troubleshooting

**Gateway won't start**: Check port 8000 is free, schema.scl path is correct

**Simulation can't connect**: Verify gateway is running, check WebSocket URL

**LLM client errors**: Check API key, base URL, model name in .env

**No LLM requests**: Check simulation.as is running, `LLM_ANALYSIS_INTERVAL_S` setting

**Actions not applied**: Check confidence threshold (70%), verify simulation reads LLMAnalysis DB