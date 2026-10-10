#!/usr/bin/env python3
"""
Mock LLM Provider for LLM-in-the-loop experiment.

This is a simple always-available provider that generates deterministic
responses based on plant state - no API key or network required.

Uses the binary memory interface to write String fields properly.
"""

import asyncio
import json
import time
import sys
import os

# Add sgrn python to path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'sgrn', 'python'))

from sgrn.gateway import Gateway
import numpy as np


GATEWAY_URL = "http://127.0.0.1:8000"
POLL_INTERVAL = 1.0


class MockLLMProvider:
    """Simple rule-based LLM that responds based on plant state."""
    
    def __init__(self):
        self.request_id = 0
        self.last_response = ""
        self.gw = Gateway(GATEWAY_URL)
        self._init_schema()
        
    def _init_schema(self):
        """Initialize schema for binary writes."""
        reg = self.gw.registry()
        self.db_schema = reg.dbByName('LLMAnalysis')
        self.dt = self.db_schema.toDtype(t_udts=reg.udtsByName())
        
    def read_db(self, db_name: str) -> dict:
        """Read DB via REST API."""
        return self.gw.readData(db_name)
    
    def write_db_binary(self, data: dict) -> bool:
        """Write LLMAnalysis DB via binary memory interface to support String fields."""
        try:
            # Read current state
            raw = self.gw.memoryRead(self.db_schema.db_number, 0, self.db_schema.size_bytes)
            rec = np.frombuffer(raw, dtype=self.dt, count=1)[0].copy()
            
            # Update response struct
            response = rec['response']
            response['analysis_text'] = data['response']['analysis_text'].encode('utf-8')
            response['suggested_action'] = data['response']['suggested_action']
            response['action_confidence'] = data['response']['action_confidence']
            response['reasoning'] = data['response']['reasoning'].encode('utf-8')
            response['timestamp_ms'] = data['response']['timestamp_ms']
            response['model_name'] = data['response']['model_name'].encode('utf-8')
            
            # Update flags
            rec['request_pending'] = data['request_pending']
            rec['last_error'] = data.get('last_error', '').encode('utf-8')
            
            # Write back
            self.gw.memoryWrite(self.db_schema.db_number, 0, rec.tobytes())
            return True
        except Exception as e:
            print(f"[MockLLM] Binary write error: {e}")
            return False
    
    def analyze(self, state: dict) -> dict:
        """Generate analysis and action based on plant state."""
        tank = state.get("Tank", {})
        pump = state.get("Pump", {})
        valves = state.get("Valves", {})
        heater = state.get("Heater", {})
        alarms = state.get("Alarms", {})
        setpoints = state.get("Setpoints", {})
        
        level = tank.get("level", 50)
        temp = tank.get("temperature", 25)
        inlet_flow = tank.get("inlet_flow", 0)
        outlet_flow = tank.get("outlet_flow", 0)
        pump_running = pump.get("running", False)
        pump_speed = pump.get("speed", 0)
        heater_enabled = heater.get("loop", {}).get("enabled", False)
        heater_sp = heater.get("loop", {}).get("setpoint", 60)
        e_stop = setpoints.get("e_stop", False)
        high_alarm = alarms.get("high_level", False)
        low_alarm = alarms.get("low_level", False)
        any_alarm = alarms.get("any_active", False)
        
        # Determine action based on simple rules
        action = 0  # HOLD
        confidence = 50
        reasoning = "Monitoring"
        
        if e_stop or any_alarm:
            action = 7  # EMERGENCY_STOP
            confidence = 95
            reasoning = "Emergency condition detected"
        elif high_alarm:
            action = 2  # DECREASE_FILL
            confidence = 90
            reasoning = "High level alarm - stop filling"
        elif low_alarm:
            action = 1  # INCREASE_FILL
            confidence = 85
            reasoning = "Low level alarm - increase fill"
        elif temp < heater_sp - 5 and tank.get("volume", 0) > 100:
            action = 3  # START_HEAT
            confidence = 80
            reasoning = f"Temp {temp:.1f}°C below setpoint {heater_sp}°C"
        elif temp > heater_sp + 2 and heater_enabled:
            action = 4  # STOP_HEAT
            confidence = 75
            reasoning = f"Temp {temp:.1f}°C at/above setpoint"
        elif level < 30 and not pump_running:
            action = 1  # INCREASE_FILL
            confidence = 75
            reasoning = f"Level {level:.1f}% low - start filling"
        elif level > 70 and pump_running:
            action = 2  # DECREASE_FILL
            confidence = 70
            reasoning = f"Level {level:.1f}% high - reduce fill"
        elif level > 60 and not pump_running and outlet_flow < 5:
            action = 5  # OPEN_OUTLET
            confidence = 65
            reasoning = f"Level {level:.1f}% ready for discharge"
        else:
            action = 0
            confidence = 60
            reasoning = f"Normal: level={level:.1f}%, temp={temp:.1f}°C"
        
        analysis = (f"Plant state: Level {level:.1f}%, Temp {temp:.1f}°C, "
                   f"In={inlet_flow:.1f} L/min, Out={outlet_flow:.1f} L/min. "
                   f"Pump {'RUN' if pump_running else 'STOP'} @ {pump_speed:.0f}%. "
                   f"Heater {'ON' if heater_enabled else 'OFF'} (SP={heater_sp:.0f}°C). "
                   f"Alarms: {'ACTIVE' if any_alarm else 'None'}.")
        
        return {
            "analysis": analysis,
            "action": action,
            "confidence": confidence,
            "reasoning": reasoning
        }
    
    async def run(self):
        print("[MockLLM] Starting mock LLM provider (binary mode)...")
        
        while True:
            try:
                llm_db = self.read_db("LLMAnalysis")
                if not llm_db:
                    await asyncio.sleep(POLL_INTERVAL)
                    continue
                
                if llm_db.get("request_pending") and llm_db.get("request_id", 0) != self.request_id:
                    self.request_id = llm_db.get("request_id", 0)
                    print(f"[MockLLM] Processing request ID {self.request_id}")
                    
                    # Read all plant state
                    state = {}
                    for db in ["Tank", "Pump", "Valves", "Heater", "Alarms", "Setpoints"]:
                        state[db] = self.read_db(db)
                    
                    # Generate response
                    result = self.analyze(state)
                    
                    # Write response via binary interface
                    response_data = {
                        "response": {
                            "analysis_text": result["analysis"],
                            "suggested_action": result["action"],
                            "action_confidence": result["confidence"],
                            "reasoning": result["reasoning"],
                            "timestamp_ms": int(time.time() * 1000) & 0x7FFFFFFF,  # Clamp to int32
                            "model_name": "mock-llm-v1"
                        },
                        "request_pending": False,
                        "last_error": ""
                    }
                    
                    success = self.write_db_binary(response_data)
                    if success:
                        self.last_response = f"Action {result['action']} ({result['confidence']}%)"
                        print(f"[MockLLM] Response sent: {self.last_response}")
                    else:
                        print(f"[MockLLM] Failed to write response")
                
                await asyncio.sleep(POLL_INTERVAL)
                
            except KeyboardInterrupt:
                print("\n[MockLLM] Shutting down...")
                break
            except Exception as e:
                print(f"[MockLLM] Error: {e}")
                import traceback
                traceback.print_exc()
                await asyncio.sleep(POLL_INTERVAL)


async def main():
    provider = MockLLMProvider()
    await provider.run()


if __name__ == "__main__":
    asyncio.run(main())