#!/usr/bin/env python3
"""
Complete LLM-in-the-loop example using LangChain + OpenRouter + SGRN Gateway tools.

This replaces the original `llm_client.py` with a proper ReAct agent that:
1. Discovers the plant schema via registry tools
2. Reads multi-DB plant state
3. Reasons about the correct action
4. Writes the action back via tool calls
5. Closes the loop automatically

Run:
    cd experiments/llm_in_loop
    cp .env.example .env
    # Edit .env with your OPENROUTER_API_KEY
    python langchain_agent_loop.py
"""

from __future__ import annotations

import os
import sys
import time

from dotenv import load_dotenv

# Ensure we can import from sgrn.python
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "sgrn", "python"))

from langchain_tools import create_openrouter_llm, create_agent_executor, get_sgrn_tools


# ──────────────────────────────────────────────────────────────────────────────
# System Prompt for Process Control Agent
# ──────────────────────────────────────────────────────────────────────────────

PROCESS_CONTROL_SYSTEM_PROMPT = """You are an expert process control engineer operating a tank/pump/heater skid via the SGRN Gateway digital twin.

PLANT OVERVIEW (from schema.scl):
- DB1 Setpoints: Operator commands (pump_speed_sp, inlet_valve_sp, outlet_valve_sp, heater_enabled_sp, heater_setpoint_sp, e_stop)
- DB2 Tank: Process values (level_pct, volume_l, temp_c, inlet_flow_lpm, outlet_flow_lpm, alarms)
- DB3 Pump: Status (running, speed_pct, fault)
- DB4 Valves: Actuator positions (inlet.position_pct, outlet.position_pct)
- DB5 Heater: PID loop (loop.enabled, loop.output_pct, loop.setpoint)
- DB6 Alarms: Aggregated status (any_active)
- DB7 LLMAnalysis: Request/response protocol (request_pending, request_id, response.{analysis_text,suggested_action,action_confidence})

AVAILABLE ACTIONS (written to Setpoints DB):
- pump_speed_sp: 0-100% (pump speed setpoint)
- inlet_valve_sp: 0-100% (inlet valve position setpoint)
- outlet_valve_sp: 0-100% (outlet valve position setpoint)
- heater_enabled_sp: true/false (heater enable)
- heater_setpoint_sp: temperature in °C (heater setpoint)
- e_stop: true/false (emergency stop)

NORMAL OPERATING SEQUENCE:
1. FILL: Pump on, inlet valve open, fill to ~70% level
2. HEAT: Heater on, setpoint 60°C, hold until temp reached
3. DISCHARGE: Outlet valve open, pump on, drain to ~20%
4. Repeat or HOLD

SAFETY RULES (MUST FOLLOW):
- NEVER exceed 90% tank level (overfill risk)
- NEVER run pump dry (level < 10%)
- NEVER exceed 80°C tank temperature
- If any alarm active → safe shutdown (pump off, valves closed, heater off)
- E-stop engaged → all outputs zero, require manual reset

YOUR TOOLS:
- list_databases: Quick overview of DBs
- get_registry: Full schema with field details (use once at start)
- read_plant_data: Read any DB/field (e.g., "Tank", "Setpoints/pump_speed_sp")
- write_plant_data: Write to Setpoints DB with actuator commands

LOOP PROTOCOL:
The simulation sets LLMAnalysis.request_pending=true every 10 seconds with a new request_id.
Your job: detect pending request → read plant state → decide action → write to Setpoints → 
write response to LLMAnalysis with analysis_text, suggested_action (0-7), action_confidence, reasoning.

ACTION CODES for LLMAnalysis.response.suggested_action:
0=HOLD, 1=INCREASE_FILL, 2=DECREASE_FILL, 3=START_HEAT, 4=STOP_HEAT, 5=OPEN_OUTLET, 6=CLOSE_OUTLET, 7=EMERGENCY_STOP

WORKFLOW EACH CYCLE:
1. read_plant_data("LLMAnalysis") → check request_pending & request_id
2. If new request: read_plant_data("Tank"), read_plant_data("Pump"), read_plant_data("Valves"), read_plant_data("Heater"), read_plant_data("Setpoints"), read_plant_data("Alarms")
3. Analyze state, decide action
4. write_plant_data("Setpoints", {appropriate actuator commands})
5. write_plant_data("LLMAnalysis", {request_pending: false, response: {analysis_text, suggested_action, action_confidence, reasoning, timestamp_ms, model_name}})
6. Wait for next cycle
"""


# ──────────────────────────────────────────────────────────────────────────────
# Agent Loop
# ──────────────────────────────────────────────────────────────────────────────

def run_agent_loop(poll_interval: float = 2.0, max_cycles: int = 0):
    """Run the LLM agent in a continuous control loop."""
    
    load_dotenv()
    
    api_key = os.getenv("OPENROUTER_API_KEY")
    if not api_key:
        print("ERROR: OPENROUTER_API_KEY not set in .env")
        return
    
    gateway_url = os.getenv("SGRN_GATEWAY_URL", "http://localhost:8000")
    os.environ["SGRN_GATEWAY_URL"] = gateway_url  # for tools
    
    print(f"[Agent] Connecting to gateway at {gateway_url}")
    print(f"[Agent] Using OpenRouter model")
    
    # Create LLM and agent
    llm = create_openrouter_llm(
        model=os.getenv("OPENROUTER_MODEL", "openrouter/auto"),
        temperature=0.1,
    )
    
    # We'll use a custom prompt with the agent executor
    from langchain.agents import AgentExecutor, create_openai_tools_agent
    from langchain_core.prompts import ChatPromptTemplate, MessagesPlaceholder
    
    tools = get_sgrn_tools()
    
    prompt = ChatPromptTemplate.from_messages([
        ("system", PROCESS_CONTROL_SYSTEM_PROMPT),
        ("human", "{input}"),
        MessagesPlaceholder(variable_name="agent_scratchpad"),
    ])
    
    agent = create_openai_tools_agent(llm, tools, prompt)
    executor = AgentExecutor(agent=agent, tools=tools, verbose=True, max_iterations=8)
    
    last_request_id = 0
    cycle = 0
    
    print("[Agent] Starting control loop...")
    print("[Agent] Press Ctrl+C to stop\n")
    
    try:
        while max_cycles == 0 or cycle < max_cycles:
            cycle += 1
            print(f"\n{'='*60}")
            print(f"[Agent] Cycle {cycle} - Checking for LLM request...")
            print(f"{'='*60}")
            
            # Step 1: Check for pending request
            try:
                llm_db = executor.invoke({
                    "input": "Read the LLMAnalysis DB to check if there's a pending request. Return the raw JSON."
                })
                # The agent will call read_plant_data("LLMAnalysis") tool
                # We need to parse the result - but the agent returns text
                # Better: call the tool directly for the check
            except Exception as e:
                print(f"[Agent] Error checking LLMAnalysis: {e}")
                time.sleep(poll_interval)
                continue
            
            # Direct tool call for the check (more reliable than agent for this)
            from langchain_tools import ReadPlantDataTool
            read_tool = ReadPlantDataTool()
            llm_analysis = read_tool.invoke({"path": "LLMAnalysis"})
            
            import json
            try:
                llm_data = json.loads(llm_analysis)
            except:
                print(f"[Agent] Could not parse LLMAnalysis: {llm_analysis}")
                time.sleep(poll_interval)
                continue
            
            request_pending = llm_data.get("request_pending", False)
            request_id = llm_data.get("request_id", 0)
            
            if not request_pending or request_id == last_request_id:
                print(f"[Agent] No new request (pending={request_pending}, id={request_id}, last={last_request_id})")
                time.sleep(poll_interval)
                continue
            
            print(f"[Agent] NEW REQUEST detected: request_id={request_id}")
            last_request_id = request_id
            
            # Step 2: Read all plant state
            print("[Agent] Reading plant state...")
            plant_state = {}
            for db in ["Tank", "Pump", "Valves", "Heater", "Setpoints", "Alarms"]:
                result = read_tool.invoke({"path": db})
                try:
                    plant_state[db] = json.loads(result)
                except:
                    plant_state[db] = {"error": result}
            
            # Step 3: Build analysis prompt for the agent
            analysis_prompt = f"""LLM Analysis Request #{request_id}

CURRENT PLANT STATE:
{json.dumps(plant_state, indent=2)}

TASK: Analyze the plant state and:
1. Write appropriate actuator commands to the Setpoints DB
2. Write the analysis response to LLMAnalysis DB with:
   - request_pending: false
   - response: {{
       analysis_text: "your 2-3 sentence analysis",
       suggested_action: <0-7 action code>,
       action_confidence: <0-100>,
       reasoning: "why this action",
       timestamp_ms: <current unix ms>,
       model_name: "openrouter/<model>"
     }}

Use the write_plant_data tool for both writes."""
            
            # Step 4: Let the agent decide and act
            print("[Agent] Invoking LLM to analyze and act...")
            result = executor.invoke({"input": analysis_prompt})
            print(f"[Agent] Cycle {cycle} complete: {result['output'][:200]}...")
            
            time.sleep(poll_interval)
            
    except KeyboardInterrupt:
        print("\n[Agent] Shutdown requested")
    except Exception as e:
        print(f"[Agent] Fatal error: {e}")
        import traceback
        traceback.print_exc()


if __name__ == "__main__":
    run_agent_loop()