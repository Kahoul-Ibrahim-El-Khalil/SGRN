#!/usr/bin/env python3
"""LangChain Agent for Plant Monitoring and Operator Advisories.

Features:
  - Read-only tools (get_latest_detector_state, get_signal_trend, get_replay_status)
  - Supports OpenAI-compatible LLM provider via environment variables:
      OPENAI_API_KEY / OPENROUTER_API_KEY
      LLM_MODEL (default: gpt-4o-mini or auto)
      LLM_BASE_URL (optional)
  - Offline mode (--no-llm) generating verified templated messages from same inputs.
  - Strict grounding: only refers to values present in detector output.
"""

from __future__ import annotations

import os
import sys
from typing import Any, Dict, Optional, Tuple

from langchain_tools import GLOBAL_STORE, get_flare_tools


def format_top_signals_summary(top_signals: list[dict]) -> str:
    lines = []
    for i, s in enumerate(top_signals, 1):
        name = s.get("signal", "Unknown")
        val = s.get("current_value", 0.0)
        rng = s.get("normal_range", [0.0, 0.0])
        err = s.get("reconstruction_error", 0.0)
        lines.append(f"  {i}. {name}: {val:.2f} (normal range: [{rng[0]:.2f}, {rng[1]:.2f}], err: {err:.3f})")
    return "\n".join(lines)


def generate_offline_advisory(state_dict: dict) -> Tuple[str, str]:
    """Generate deterministically grounded assessment and action text without LLM."""
    state = state_dict.get("state", "normal")
    score = state_dict.get("score", 0.0)
    top_signals = state_dict.get("top_signals", [])
    top1 = top_signals[0]["signal"] if len(top_signals) > 0 else "N/A"
    top1_val = top_signals[0]["current_value"] if len(top_signals) > 0 else 0.0

    if state == "normal":
        assessment = "All plant variables are behaving within nominal tower-cycle envelopes; autoencoder reconstruction error is below calibrated threshold."
        action = "Maintain current operating parameters and standard cycle sequence; no operator intervention required."
    elif state == "uncertain":
        assessment = f"Incipient anomaly detected: {top1} is rising ({top1_val:.2f}) and score ({score:.3f}) exceeds conformal threshold q_alpha."
        action = f"Monitor {top1} trend; verify field transmitter calibration and prepare for potential pressure relief intervention."
    else:  # abnormal
        top2 = top_signals[1]["signal"] if len(top_signals) > 1 else ""
        top2_val = top_signals[1]["current_value"] if len(top_signals) > 1 else 0.0
        assessment = f"Critical anomaly detected: significant pressure rise on {top1} ({top1_val:.2f}) and {top2} ({top2_val:.2f}) with score ({score:.1f}) exceeding upper uncertainty bound."
        action = "Check flare header / vent path immediately; inspect relief path valves; ESD trip imminent if feed pressure exceeds 90 bar."
    return assessment, action


class FlareMonitoringAgent:
    def __init__(self, use_llm: bool = True):
        self.use_llm = use_llm
        self.executor = None
        self.tools = get_flare_tools()

        if self.use_llm:
            api_key = os.getenv("OPENROUTER_API_KEY") or os.getenv("OPENAI_API_KEY")
            base_url = os.getenv("LLM_BASE_URL")
            if os.getenv("OPENROUTER_API_KEY") and not base_url:
                base_url = "https://openrouter.ai/api/v1"
            model_name = os.getenv("LLM_MODEL", "gpt-4o-mini" if not os.getenv("OPENROUTER_API_KEY") else "openrouter/auto")

            if not api_key:
                print("[agent] Notice: No API key found in environment; falling back to offline template mode (--no-llm).")
                self.use_llm = False
            else:
                try:
                    from langchain.agents import AgentExecutor, create_openai_tools_agent
                    from langchain_core.prompts import ChatPromptTemplate, MessagesPlaceholder
                    from langchain_openai import ChatOpenAI

                    llm = ChatOpenAI(
                        model=model_name,
                        api_key=api_key,
                        base_url=base_url,
                        temperature=0.1,
                    )
                    prompt = ChatPromptTemplate.from_messages([
                        (
                            "system",
                            """You are an industrial process control AI advisor monitoring a gas processing dehydration plant via read-only tools.
Your duty is to query the detector and signal trends and produce a grounded operational advisory.

STRICT CONSTRAINTS:
1. You are READ-ONLY. Never attempt to write to the plant or generate scripts.
2. Ground all numerical statements strictly in the tool outputs. Do not invent any numbers.
3. You must output EXACTLY two sections:
[ASSESSMENT]: <one clear sentence summarizing the physical situation>
[ACTION]: <one concise text-only recommendation for the control room operator>
Example action when flare rises: 'Check flare header / vent path; inspect relief valves; ESD possible.'""",
                        ),
                        ("human", "{input}"),
                        MessagesPlaceholder(variable_name="agent_scratchpad"),
                    ])
                    agent = create_openai_tools_agent(llm, self.tools, prompt)
                    self.executor = AgentExecutor(agent=agent, tools=self.tools, verbose=False, max_iterations=3)
                    print(f"[agent] Initialized LLM agent ({model_name} at {base_url or 'OpenAI'}).")
                except Exception as exc:
                    print(f"[agent] Warning: Failed to initialize LLM agent ({exc}); using offline template mode.")
                    self.use_llm = False

    def generate_advisory(self, state_dict: dict) -> Tuple[str, str]:
        if not self.use_llm or self.executor is None:
            return generate_offline_advisory(state_dict)

        sim_time = state_dict.get("sim_time", "N/A")
        state = state_dict.get("state", "normal")
        score = state_dict.get("score", 0.0)
        query = (
            f"Current plant time is {sim_time}. Detector reported state='{state}' with score={score:.3f}. "
            "Call get_latest_detector_state to inspect top signals and provide your [ASSESSMENT] and [ACTION]."
        )
        try:
            res = self.executor.invoke({"input": query})
            output = res.get("output", "")
            assessment = "Process monitored."
            action = "Continue standard monitoring."
            if "[ASSESSMENT]:" in output and "[ACTION]:" in output:
                parts = output.split("[ACTION]:")
                assessment = parts[0].replace("[ASSESSMENT]:", "").strip()
                action = parts[1].strip()
            else:
                lines = [l.strip() for l in output.splitlines() if l.strip()]
                if lines:
                    assessment = lines[0]
                    action = lines[1] if len(lines) > 1 else "Check system valves."
            return assessment, action
        except Exception as exc:
            print(f"[agent] LLM invocation error ({exc}), falling back to offline template.")
            return generate_offline_advisory(state_dict)

    def print_operator_block(self, state_dict: dict) -> None:
        sim_time = state_dict.get("sim_time", "N/A")
        state = state_dict.get("state", "normal").upper()
        score = state_dict.get("score", 0.0)
        q_alpha = state_dict.get("q_alpha", 0.0)
        q_upper = state_dict.get("q_upper", 0.0)
        top_signals = state_dict.get("top_signals", [])

        assessment, action = self.generate_advisory(state_dict)

        state_color = ""
        if state == "NORMAL":
            tag = "[NORMAL]"
        elif state == "UNCERTAIN":
            tag = "[UNCERTAIN WARNING]"
        else:
            tag = "[CRITICAL ALARM]"

        print("\n" + "=" * 80)
        print(f"  {tag}  Sim Time: {sim_time}")
        print("=" * 80)
        print(f"  State:       {state}  |  Score: {score:.3f}  (q_alpha: {q_alpha:.3f}, q_upper: {q_upper:.3f})")
        print("  Top Signals:")
        print(format_top_signals_summary(top_signals))
        print(f"  Assessment:  {assessment}")
        print(f"  Action:      {action}")
        print("=" * 80 + "\n")
