#!/usr/bin/env python3
"""
LangChain tools for SGRN Gateway — enables LLM agents to:
  • Discover the schema registry (DBs, UDTs, tags)
  • Read plant state from any DB/field
  • Write actions/actuator values back to the twin
  • Query type dictionary & Modbus map

Compatible with any OpenAI-compatible endpoint (OpenRouter, Groq, Ollama, vLLM, etc.)
via `langchain_openai.ChatOpenAI`.
"""

from __future__ import annotations

import json
import os
from typing import Any, Optional, Type

from langchain_core.callbacks import CallbackManagerForToolRun
from langchain_core.tools import BaseTool
from langchain_openai import ChatOpenAI
from pydantic import BaseModel, Field

# ──────────────────────────────────────────────────────────────────────────────
# Gateway client (re-uses the existing dependency-free sgrn.python.sgrn.gateway)
# ──────────────────────────────────────────────────────────────────────────────

try:
    from sgrn.gateway import Gateway, GatewayError
except ImportError:
    # Allow running from experiments/llm_in_loop without install
    import sys
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "sgrn", "python"))
    from sgrn.gateway import Gateway, GatewayError


# ──────────────────────────────────────────────────────────────────────────────
# Shared gateway instance (lazy-initialized)
# ──────────────────────────────────────────────────────────────────────────────

_GATEWAY: Optional[Gateway] = None


def getGateway() -> Gateway:
    global _GATEWAY
    if _GATEWAY is None:
        base_url = os.getenv("SGRN_GATEWAY_URL", "http://localhost:8000")
        _GATEWAY = Gateway(base_url)
    return _GATEWAY


# ──────────────────────────────────────────────────────────────────────────────
# Tool Input Schemas
# ──────────────────────────────────────────────────────────────────────────────

class RegistryInput(BaseModel):
    headers_only: bool = Field(default=False, description="Return only DB headers (faster, no field trees)")
    db_number: Optional[int] = Field(default=None, description="Restrict to a single DB by number")


class ReadDataInput(BaseModel):
    path: str = Field(description="Data path, e.g. 'Tank', 'Tank/level_pct', 'DB2/temperatures/3'")


class WriteDataInput(BaseModel):
    path: str = Field(description="Data path (DB name or field path), e.g. 'Setpoints', 'Setpoints/pump_speed_sp'")
    fields: dict = Field(description="Fields to write as JSON object, e.g. {'pump_speed_sp': 50.0}")


class ListDBsInput(BaseModel):
    pass  # no arguments needed


# ──────────────────────────────────────────────────────────────────────────────
# Tool Implementations
# ──────────────────────────────────────────────────────────────────────────────

class GetRegistryTool(BaseTool):
    """Fetch the full schema registry (all DBs, UDTs, tags) from the gateway."""
    name: str = "get_registry"
    description: str = (
        "Get the complete SGRN Gateway schema registry. Returns all data blocks (DBs), "
        "user-defined types (UDTs), and their field definitions. Use this first to discover "
        "what DBs and fields are available before reading/writing data."
    )
    args_schema: Type[BaseModel] = RegistryInput

    def _run(
        self,
        headers_only: bool = False,
        db_number: Optional[int] = None,
        run_manager: Optional[CallbackManagerForToolRun] = None,
    ) -> str:
        gw = getGateway()
        try:
            reg = gw.registry(t_headers_only=headers_only, t_db=db_number)
            # Return a concise summary for the LLM (full JSON can be huge)
            if headers_only:
                summary = {
                    "databases": [
                        {"db_number": db.db_number, "db_name": db.db_name, "size_bytes": db.size_bytes}
                        for db in reg.dbs
                    ]
                }
            else:
                summary = {
                    "databases": [
                        {
                            "db_number": db.db_number,
                            "db_name": db.db_name,
                            "size_bytes": db.size_bytes,
                            "fields": [
                                {"name": f.name, "type": f.type_name, "offset": f.offset, "array_length": f.array_length}
                                for f in db.fields
                            ] if db.fields else []
                        }
                        for db in reg.dbs
                    ],
                    "udt_count": len(reg.udts) if reg.udts else 0,
                }
            return json.dumps(summary, indent=2)
        except GatewayError as e:
            return f"Gateway error: {e}"
        except Exception as e:
            return f"Error fetching registry: {e}"


class ListDatabasesTool(BaseTool):
    """List all available data blocks (DBs) with their numbers and names."""
    name: str = "list_databases"
    description: str = "List all data blocks (DBs) in the gateway with their numbers, names, and sizes. Quick overview without full field details."
    args_schema: Type[BaseModel] = ListDBsInput

    def _run(self, run_manager: Optional[CallbackManagerForToolRun] = None) -> str:
        gw = getGateway()
        try:
            reg = gw.registry(t_headers_only=True)
            dbs = [
                {"db_number": db.db_number, "db_name": db.db_name, "size_bytes": db.size_bytes}
                for db in reg.dbs
            ]
            return json.dumps({"databases": dbs}, indent=2)
        except Exception as e:
            return f"Error listing databases: {e}"


class ReadPlantDataTool(BaseTool):
    """Read current plant state from the digital twin (full DB, subtree, or single field)."""
    name: str = "read_plant_data"
    description: str = (
        "Read live plant data from the SGRN Gateway digital twin. "
        "Provide a path like 'Tank' (full DB), 'Tank/level_pct' (single field), "
        "or 'DB2/temperatures/3' (array element). Returns current values as JSON."
    )
    args_schema: Type[BaseModel] = ReadDataInput

    def _run(self, path: str, run_manager: Optional[CallbackManagerForToolRun] = None) -> str:
        gw = getGateway()
        try:
            data = gw.readData(path)
            return json.dumps(data, indent=2, default=str)
        except GatewayError as e:
            return f"Gateway error reading {path}: {e}"
        except Exception as e:
            return f"Error reading plant data: {e}"


class WritePlantDataTool(BaseTool):
    """Write actuator values / setpoints / actions to the digital twin."""
    name: str = "write_plant_data"
    description: str = (
        "Write values to the SGRN Gateway digital twin (actuators, setpoints, commands). "
        "Provide a path (DB name or field path) and a JSON object of fields to merge-write. "
        "Example: path='Setpoints', fields={'pump_speed_sp': 50.0, 'inlet_valve_sp': 30.0}. "
        "Returns write confirmation."
    )
    args_schema: Type[BaseModel] = WriteDataInput

    def _run(self, path: str, fields: dict, run_manager: Optional[CallbackManagerForToolRun] = None) -> str:
        gw = getGateway()
        try:
            result = gw.writeFields(path, fields)
            return json.dumps({"success": True, "result": result}, indent=2)
        except GatewayError as e:
            return f"Gateway error writing {path}: {e}"
        except Exception as e:
            return f"Error writing plant data: {e}"


class GetTypeDictionaryTool(BaseTool):
    """Get the S7 type dictionary (UDT definitions, primitive types)."""
    name: str = "get_type_dictionary"
    description: str = "Fetch the S7 type dictionary from the gateway — all user-defined types (UDTs) and primitive type definitions with their byte layouts."
    args_schema: Type[BaseModel] = ListDBsInput

    def _run(self, run_manager: Optional[CallbackManagerForToolRun] = None) -> str:
        gw = getGateway()
        try:
            types = gw.registryTypes()
            return json.dumps(types, indent=2, default=str)
        except Exception as e:
            return f"Error fetching type dictionary: {e}"


class GetModbusMapTool(BaseTool):
    """Get the Modbus virtual register map."""
    name: str = "get_modbus_map"
    description: str = "Fetch the Modbus virtual register map from the gateway (if Modbus is enabled). Maps tag paths to Modbus register addresses."
    args_schema: Type[BaseModel] = ListDBsInput

    def _run(self, run_manager: Optional[CallbackManagerForToolRun] = None) -> str:
        gw = getGateway()
        try:
            modbus = gw.modbusMap()
            return json.dumps(modbus, indent=2, default=str)
        except Exception as e:
            return f"Error fetching Modbus map: {e}"


# ──────────────────────────────────────────────────────────────────────────────
# Tool Collection Factory
# ──────────────────────────────────────────────────────────────────────────────

def getSgrnTools() -> list[BaseTool]:
    """Return all SGRN Gateway tools for use with a LangChain agent."""
    return [
        GetRegistryTool(),
        ListDatabasesTool(),
        ReadPlantDataTool(),
        WritePlantDataTool(),
        GetTypeDictionaryTool(),
        GetModbusMapTool(),
    ]


# ──────────────────────────────────────────────────────────────────────────────
# Example: Building the LLM Agent with OpenRouter
# ──────────────────────────────────────────────────────────────────────────────

def createOpenrouterLlm(
    model: str = "openrouter/auto",  # or specific model like "anthropic/claude-3.5-sonnet"
    temperature: float = 0.1,
) -> ChatOpenAI:
    """
    Create a ChatOpenAI instance configured for OpenRouter.
    
    Requires OPENROUTER_API_KEY in environment.
    """
    api_key = os.getenv("OPENROUTER_API_KEY")
    if not api_key:
        raise ValueError("OPENROUTER_API_KEY not set in environment")
    
    return ChatOpenAI(
        model=model,
        base_url="https://openrouter.ai/api/v1",
        api_key=api_key,
        temperature=temperature,
    )


def createAgentExecutor(llm: ChatOpenAI):
    """Create a LangChain agent executor with SGRN tools."""
    from langchain.agents import AgentExecutor, create_openai_tools_agent
    from langchain_core.prompts import ChatPromptTemplate, MessagesPlaceholder
    
    tools = getSgrnTools()
    
    prompt = ChatPromptTemplate.from_messages([
        ("system", """You are an expert process control engineer operating an industrial digital twin via the SGRN Gateway.

You have access to tools to:
1. Discover the schema registry (list DBs, get full registry, type dictionary)
2. Read live plant state from any data block or field
3. Write actuator commands, setpoints, and control actions back to the twin

WORKFLOW:
- Start by exploring the registry to understand available DBs and fields
- Read relevant plant state to assess the situation
- Write actions to appropriate DBs (typically Setpoints, Supervisor, or similar control DBs)
- Confirm writes succeeded by reading back

SAFETY RULES:
- Never write to safety-critical fields (ESD, trips) without explicit confirmation
- Prefer incremental changes over large steps
- Always read current state before writing
- If uncertain, ask for clarification rather than guessing"""),
        ("human", "{input}"),
        MessagesPlaceholder(variable_name="agent_scratchpad"),
    ])
    
    agent = create_openai_tools_agent(llm, tools, prompt)
    return AgentExecutor(agent=agent, tools=tools, verbose=True, max_iterations=10)


# ──────────────────────────────────────────────────────────────────────────────
# Demo / CLI Entry Point
# ──────────────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    import sys
    from dotenv import load_dotenv
    
    load_dotenv()
    
    if not os.getenv("OPENROUTER_API_KEY"):
        print("ERROR: OPENROUTER_API_KEY not set. Create a .env file or export it.")
        sys.exit(1)
    
    # Quick test: list databases
    print("=" * 60)
    print("Testing SGRN Gateway tools...")
    print("=" * 60)
    
    tool = ListDatabasesTool()
    result = tool.invoke({})
    print(result)
    
    print("\n" + "=" * 60)
    print("Creating LLM agent with OpenRouter...")
    print("=" * 60)
    
    llm = createOpenrouterLlm()
    agent = createAgentExecutor(llm)
    
    # Example task
    task = (
        "Explore the registry to understand the plant schema, "
        "then read the current state of the Tank and Pump DBs, "
        "and tell me what the current operating mode appears to be."
    )
    
    print(f"\nTask: {task}\n")
    result = agent.invoke({"input": task})
    print(f"\nResult: {result['output']}")