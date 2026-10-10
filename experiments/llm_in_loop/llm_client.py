#!/usr/bin/env python3
"""
LLM-in-the-loop Python script for SGRN Tank/Pump Skid simulation.

This script:
1. Connects to the SGRN gateway via WebSocket
2. Monitors the LLMAnalysis DB for pending analysis requests
3. When a request is pending, reads current plant state from all DBs
4. Calls an LLM (via OpenAI-compatible API) with plant state context
5. Writes the LLM's analysis and suggested action back to LLMAnalysis DB

Requirements:
- pip install websockets openai python-dotenv

Environment variables (or .env file):
- LLM_API_KEY: API key for LLM provider
- LLM_BASE_URL: Base URL for OpenAI-compatible API (default: https://api.openai.com/v1)
- LLM_MODEL: Model name (default: gpt-4o-mini)
- GATEWAY_WS_URL: WebSocket URL for gateway (default: ws://127.0.0.1:8000/ws)
"""

import asyncio
import json
import os
import sys
import time
from dataclasses import dataclass
from typing import Optional

import websockets
from openai import AsyncOpenAI
from dotenv import load_dotenv

# Load environment variables
load_dotenv()

# Configuration
GATEWAY_WS_URL = os.getenv("GATEWAY_WS_URL", "ws://127.0.0.1:8000/ws")
LLM_API_KEY = os.getenv("LLM_API_KEY")
LLM_BASE_URL = os.getenv("LLM_BASE_URL", "https://api.openai.com/v1")
LLM_MODEL = os.getenv("LLM_MODEL", "gpt-4o-mini")
POLL_INTERVAL = 1.0  # seconds between DB polls

if not LLM_API_KEY:
    print("ERROR: LLM_API_KEY not set. Please set it in environment or .env file")
    sys.exit(1)


@dataclass
class PlantState:
    """Current plant state aggregated from all DBs."""
    # Tank
    tank_level_pct: float = 0.0
    tank_volume_l: float = 0.0
    tank_temp_c: float = 0.0
    inlet_flow_lpm: float = 0.0
    outlet_flow_lpm: float = 0.0
    high_level_alarm: bool = False
    low_level_alarm: bool = False
    overfill_trip: bool = False
    
    # Pump
    pump_running: bool = False
    pump_speed_pct: float = 0.0
    pump_fault: bool = False
    
    # Valves
    inlet_valve_pct: float = 0.0
    outlet_valve_pct: float = 0.0
    
    # Heater
    heater_enabled: bool = False
    heater_on: bool = False
    heater_setpoint_c: float = 0.0
    
    # Alarms/Status
    e_stop: bool = False
    skid_mode: int = 0
    fault_active: bool = False


class LLMSimulationClient:
    def __init__(self):
        self.ws: Optional[websockets.WebSocketClientProtocol] = None
        self.llm_client = AsyncOpenAI(
            api_key=LLM_API_KEY,
            base_url=LLM_BASE_URL
        )
        self.request_id = 0
        self.plant_state = PlantState()
        self.last_llm_response = ""
        
    async def connect(self):
        """Connect to the gateway WebSocket."""
        print(f"[LLM Client] Connecting to {GATEWAY_WS_URL}...")
        self.ws = await websockets.connect(GATEWAY_WS_URL)
        print("[LLM Client] Connected!")
        
        # Subscribe to binary updates (we'll use REST for reading/writing DBs)
        # For now, we'll use the HTTP REST API to read/write DBs
        pass
    
    async def read_db(self, db_name: str) -> dict:
        """Read a data block via HTTP REST API."""
        import urllib.request
        import urllib.error
        
        url = f"http://127.0.0.1:8000/data/{db_name}"
        try:
            req = urllib.request.Request(url)
            with urllib.request.urlopen(req, timeout=5) as resp:
                return json.loads(resp.read().decode())
        except Exception as e:
            print(f"[LLM Client] Error reading {db_name}: {e}")
            return {}
    
    async def write_db(self, db_name: str, data: dict) -> bool:
        """Write a data block via HTTP REST API."""
        import urllib.request
        import urllib.error
        
        url = f"http://127.0.0.1:8000/data/{db_name}"
        try:
            req = urllib.request.Request(
                url,
                data=json.dumps(data).encode(),
                headers={"Content-Type": "application/json"},
                method="POST"
            )
            with urllib.request.urlopen(req, timeout=5) as resp:
                return resp.status == 200
        except Exception as e:
            print(f"[LLM Client] Error writing {db_name}: {e}")
            return False
    
    async def update_plant_state(self):
        """Read all DBs and update internal plant state."""
        # Read Tank (DB2)
        tank = await self.read_db("Tank")
        if tank:
            self.plant_state.tank_level_pct = tank.get("level_pct", 0.0)
            self.plant_state.tank_volume_l = tank.get("volume_l", 0.0)
            self.plant_state.tank_temp_c = tank.get("temp_c", 0.0)
            self.plant_state.inlet_flow_lpm = tank.get("inlet_flow_lpm", 0.0)
            self.plant_state.outlet_flow_lpm = tank.get("outlet_flow_lpm", 0.0)
            self.plant_state.high_level_alarm = tank.get("high_level_alarm", False)
            self.plant_state.low_level_alarm = tank.get("low_level_alarm", False)
            self.plant_state.overfill_trip = tank.get("overfill_trip", False)
        
        # Read Pump (DB3)
        pump = await self.read_db("Pump")
        if pump:
            self.plant_state.pump_running = pump.get("running", False)
            self.plant_state.pump_speed_pct = pump.get("speed_pct", 0.0)
            self.plant_state.pump_fault = pump.get("fault", False)
        
        # Read Valves (DB4)
        valves = await self.read_db("Valves")
        if valves:
            self.plant_state.inlet_valve_pct = valves.get("inlet", {}).get("position_pct", 0.0)
            self.plant_state.outlet_valve_pct = valves.get("outlet", {}).get("position_pct", 0.0)
        
        # Read Heater (DB5)
        heater = await self.read_db("Heater")
        if heater:
            loop = heater.get("loop", {})
            self.plant_state.heater_enabled = loop.get("enabled", False)
            self.plant_state.heater_on = loop.get("output_pct", 0) > 0
            self.plant_state.heater_setpoint_c = loop.get("setpoint", 0.0)
        
        # Read Setpoints (DB1) for e_stop and mode
        setpoints = await self.read_db("Setpoints")
        if setpoints:
            self.plant_state.e_stop = setpoints.get("e_stop", False)
        
        # Read Alarms (DB6) for skid mode
        alarms = await self.read_db("Alarms")
        if alarms:
            self.plant_state.fault_active = alarms.get("any_active", False)
            # Determine skid mode from alarms
            if self.plant_state.e_stop or self.plant_state.fault_active:
                self.plant_state.skid_mode = 4  # ALARM
            elif self.plant_state.heater_enabled and self.plant_state.tank_temp_c < self.plant_state.heater_setpoint_c - 1:
                self.plant_state.skid_mode = 2  # HEATING
            elif self.plant_state.pump_running and self.plant_state.inlet_flow_lpm > 1:
                self.plant_state.skid_mode = 1  # FILLING
            elif self.plant_state.outlet_flow_lpm > 1:
                self.plant_state.skid_mode = 3  # DISCHARGING
            else:
                self.plant_state.skid_mode = 0  # OFF
    
    def build_llm_prompt(self) -> str:
        """Build the prompt for the LLM with current plant state."""
        mode_names = ["OFF", "FILLING", "HEATING", "DISCHARGING", "ALARM"]
        mode_str = mode_names[self.plant_state.skid_mode] if 0 <= self.plant_state.skid_mode < 5 else "UNKNOWN"
        
        prompt = f"""You are an expert process control engineer monitoring a tank/pump/heater skid. Analyze the current plant state and recommend an action.

CURRENT PLANT STATE:
====================
Skid Mode: {mode_str}
Fault Active: {"YES" if self.plant_state.fault_active else "NO"}
Emergency Stop: {"ENGAGED" if self.plant_state.e_stop else "CLEAR"}

TANK:
  Level: {self.plant_state.tank_level_pct:.1f}% ({self.plant_state.tank_volume_l:.1f} L / 1000 L)
  Temperature: {self.plant_state.tank_temp_c:.1f}°C
  Inlet Flow: {self.plant_state.inlet_flow_lpm:.1f} L/min
  Outlet Flow: {self.plant_state.outlet_flow_lpm:.1f} L/min
  High Level Alarm: {"ACTIVE" if self.plant_state.high_level_alarm else "clear"}
  Low Level Alarm: {"ACTIVE" if self.plant_state.low_level_alarm else "clear"}
  Overfill Trip: {"TRIPPED" if self.plant_state.overfill_trip else "clear"}

PUMP:
  Running: {"YES" if self.plant_state.pump_running else "NO"}
  Speed: {self.plant_state.pump_speed_pct:.1f}%
  Fault: {"YES" if self.plant_state.pump_fault else "NO"}

VALVES:
  Inlet Valve: {self.plant_state.inlet_valve_pct:.1f}% open
  Outlet Valve: {self.plant_state.outlet_valve_pct:.1f}% open

HEATER:
  Enabled: {"YES" if self.plant_state.heater_enabled else "NO"}
  Active: {"YES" if self.plant_state.heater_on else "NO"}
  Setpoint: {self.plant_state.heater_setpoint_c:.1f}°C

PREVIOUS LLM ACTION: {self.last_llm_response if self.last_llm_response else "None"}

AVAILABLE ACTIONS:
  0 = HOLD (maintain current state)
  1 = INCREASE_FILL (increase pump speed & inlet valve)
  2 = DECREASE_FILL (decrease pump speed & inlet valve)
  3 = START_HEAT (enable heater, setpoint 60°C)
  4 = STOP_HEAT (disable heater)
  5 = OPEN_OUTLET (increase outlet valve)
  6 = CLOSE_OUTLET (decrease outlet valve)
  7 = EMERGENCY_STOP (engage E-stop)

RESPOND IN THIS EXACT JSON FORMAT:
{{
  "analysis": "Brief analysis of plant state (2-3 sentences)",
  "action": <integer 0-7>,
  "confidence": <float 0-100>,
  "reasoning": "Why this action is recommended (1-2 sentences)"
}}

Consider:
- Safety first: prevent overfill, dry-running pump, overheating
- Normal operation: fill to ~70%, heat to 60°C, then discharge
- Alarms require immediate response
- Don't oscillate; prefer HOLD unless action clearly needed"""
        
        return prompt
    
    async def call_llm(self, prompt: str) -> Optional[dict]:
        """Call the LLM and parse response."""
        try:
            print("[LLM Client] Calling LLM...")
            response = await self.llm_client.chat.completions.create(
                model=LLM_MODEL,
                messages=[
                    {"role": "system", "content": "You are a process control engineer. Respond only with valid JSON."},
                    {"role": "user", "content": prompt}
                ],
                temperature=0.1,
                max_tokens=500,
                response_format={"type": "json_object"}
            )
            
            content = response.choices[0].message.content
            result = json.loads(content)
            print(f"[LLM Client] LLM response: action={result.get('action')}, confidence={result.get('confidence')}%")
            return result
            
        except Exception as e:
            print(f"[LLM Client] LLM call failed: {e}")
            return None
    
    async def check_and_process_requests(self):
        """Check LLMAnalysis DB for pending requests and process them."""
        llm_db = await self.read_db("LLMAnalysis")
        if not llm_db:
            return
        
        request_pending = llm_db.get("request_pending", False)
        request_id = llm_db.get("request_id", 0)
        
        if request_pending and request_id != self.request_id:
            print(f"[LLM Client] New LLM request detected (ID: {request_id})")
            self.request_id = request_id
            
            # Update plant state
            await self.update_plant_state()
            
            # Build prompt and call LLM
            prompt = self.build_llm_prompt()
            llm_result = await self.call_llm(prompt)
            
            if llm_result:
                # Prepare response
                response_data = {
                    "response": {
                        "analysis_text": llm_result.get("analysis", ""),
                        "suggested_action": llm_result.get("action", 0),
                        "action_confidence": llm_result.get("confidence", 0.0),
                        "reasoning": llm_result.get("reasoning", ""),
                        "timestamp_ms": int(time.time() * 1000),
                        "model_name": LLM_MODEL
                    },
                    "request_pending": False,
                    "last_error": ""
                }
                
                # Write response back
                success = await self.write_db("LLMAnalysis", response_data)
                if success:
                    self.last_llm_response = f"Action {llm_result.get('action')} ({llm_result.get('confidence')}%)"
                    print(f"[LLM Client] Response written successfully")
                else:
                    print(f"[LLM Client] Failed to write response")
            else:
                # Write error
                error_data = {
                    "request_pending": False,
                    "last_error": "LLM call failed"
                }
                await self.write_db("LLMAnalysis", error_data)
    
    async def run(self):
        """Main loop."""
        await self.connect()
        
        print("[LLM Client] Starting main loop...")
        while True:
            try:
                await self.check_and_process_requests()
                await asyncio.sleep(POLL_INTERVAL)
            except KeyboardInterrupt:
                print("\n[LLM Client] Shutting down...")
                break
            except Exception as e:
                print(f"[LLM Client] Error in main loop: {e}")
                await asyncio.sleep(POLL_INTERVAL)
        
        if self.ws:
            await self.ws.close()


async def main():
    client = LLMSimulationClient()
    await client.run()


if __name__ == "__main__":
    asyncio.run(main())