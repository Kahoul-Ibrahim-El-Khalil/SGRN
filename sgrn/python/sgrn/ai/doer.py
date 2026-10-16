"""
sgrn.ai.doer — Grounded LLM Doer Agent for Industrial Process Operations.

Consumes semantic plant state + anomaly attribution and proposes structured actions.
Supports zero-cost APIs (Groq, HuggingFace, Gemini, OpenRouter, Ollama) and offline MockDoer.
"""

import json
import os
import urllib.request
from dataclasses import dataclass
from typing import Dict, Any, Optional, Tuple


@dataclass
class ActionProposal:
    target_db: str
    field: str
    value: Any
    explanation: str
    raw_response: str = ""


class MockDoer:
    """Deterministic mock LLM doer for offline tests and instant benchmark demos."""

    def __init__(self, mode: str = "corrective", fallback: Optional["DoerLike"] = None):
        self.mode = mode
        self.fallback = fallback

    def propose_action(
        self, semantic_state_str: str, anomaly_info: Dict[str, Any]
    ) -> ActionProposal:
        if self.fallback is not None:
            return self.fallback.propose_action(semantic_state_str, anomaly_info)
        top_attribution = anomaly_info.get("attribution", {})
        highest_err_feat = max(top_attribution.items(), key=lambda x: x[1])[0] if top_attribution else "Supervisor.plant_mode"

        if "mode" in highest_err_feat or "plant_mode" in highest_err_feat or "bearing" in str(anomaly_info):
            return ActionProposal(
                target_db="Supervisor",
                field="plant_mode",
                value=1,  # STARTING / Reset
                explanation=f"Diagnosed anomaly on {highest_err_feat}. Initiating plant recovery mode transition (STARTING = 1)."
            )
        elif "speed" in highest_err_feat or "master_speed" in highest_err_feat:
            return ActionProposal(
                target_db="Supervisor",
                field="master_speed_sp",
                value=50.0,
                explanation=f"Diagnosed speed anomaly on {highest_err_feat}. Ramping master speed demand down to 50.0% safe operating target."
            )
        else:
            return ActionProposal(
                target_db="Supervisor",
                field="plant_mode",
                value=1,
                explanation="Generic anomaly recovery action."
            )


DoerLike = Any  # structural type: anything with .propose_action(str, dict) -> ActionProposal


class LLMDoer:
    """LLM Doer Agent connecting to free/open LLM inference endpoints.

    EXPERIMENTAL: every network provider (groq, pollinations, huggingface,
    gemini, ollama-remote) transmits the full semantic plant state +
    anomaly diagnostic to a third party. The keyless ``pollinations``
    endpoint in particular sends plant state to a public service with no
    contract — do not use it for production or sensitive plants. The
    default provider is ``mock`` (offline, deterministic); opt into a
    network provider explicitly.

    Providers that require no API key (``pollinations``) are tried first so the
    agent works out of the box; ``mock`` is the offline fallback for every
    transport, auth, or malformed-JSON failure.
    """

    def __init__(
        self,
        provider: str = "mock",
        api_key: Optional[str] = None,
        model_name: Optional[str] = None,
        ollama_url: str = "http://localhost:11434",
        fallback: Optional[DoerLike] = None,
        timeout: int = 30,
    ):
        self.provider = provider.lower()
        self.api_key = api_key or os.getenv("GROQ_API_KEY") or os.getenv("HF_API_KEY") or os.getenv("GEMINI_API_KEY") or ""
        self.model_name = model_name or self._default_model_for_provider()
        self.ollama_url = ollama_url
        self.fallback = fallback
        self.timeout = timeout

    def _default_model_for_provider(self) -> str:
        if self.provider == "groq":
            return "llama-3.3-70b-versatile"
        elif self.provider in ("huggingface", "hf"):
            return "Qwen/Qwen2.5-72B-Instruct"
        elif self.provider == "gemini":
            return "gemini-1.5-flash"
        elif self.provider == "ollama":
            return "llama3.2"
        elif self.provider == "pollinations":
            return "openai-fast"
        return "mock"

    def propose_action(
        self, semantic_state_str: str, anomaly_info: Dict[str, Any]
    ) -> ActionProposal:
        if self.provider == "mock" or not self.api_key and self.provider not in ("ollama", "pollinations"):
            return MockDoer(fallback=self.fallback).propose_action(semantic_state_str, anomaly_info)

        system_prompt = (
            "You are an industrial process control AI agent operating a digital twin.\n"
            "Analyze the plant state and anomaly diagnostic below.\n"
            "Propose a corrective action in JSON with fields:\n"
            '{"target_db": "<Db>", "field": "<writable field>", "value": <new_val>, "explanation": "reason"}\n'
            "Only write fields listed as writable actuators. Reply with JSON only, no prose."
        )

        user_content = (
            f"=== CURRENT PLANT SEMANTIC STATE ===\n{semantic_state_str}\n\n"
            f"=== ANOMALY DIAGNOSTIC ===\n{json.dumps(anomaly_info, indent=2)}\n"
        )

        try:
            raw_reply = self._call_provider(system_prompt, user_content)
            parsed = self._extract_json(raw_reply)
            if not parsed.get("field"):
                raise ValueError("LLM response contained no field")
            return ActionProposal(
                target_db=parsed.get("target_db", "Supervisor"),
                field=parsed.get("field", "plant_mode"),
                value=parsed.get("value", 1),
                explanation=parsed.get("explanation", "LLM proposed action"),
                raw_response=raw_reply
            )
        except Exception as e:
            # Any failure — transport, auth, rate limit, malformed JSON —
            # degrades to the deterministic doer rather than blocking the loop.
            mock_prop = MockDoer(fallback=self.fallback).propose_action(
                semantic_state_str, anomaly_info)
            mock_prop.explanation += f" (LLM API error fallback: {str(e)})"
            return mock_prop

    def _call_provider(self, sys_prompt: str, user_prompt: str) -> str:
        if self.provider == "groq":
            url = "https://api.groq.com/openai/v1/chat/completions"
            payload = {
                "model": self.model_name,
                "messages": [
                    {"role": "system", "content": sys_prompt},
                    {"role": "user", "content": user_prompt}
                ],
                "temperature": 0.1
            }
            req = urllib.request.Request(
                url,
                data=json.dumps(payload).encode("utf-8"),
                headers={"Authorization": f"Bearer {self.api_key}", "Content-Type": "application/json"}
            )
            with urllib.request.urlopen(req, timeout=10) as resp:
                res_data = json.loads(resp.read().decode("utf-8"))
                return res_data["choices"][0]["message"]["content"]

        elif self.provider == "pollinations":
            # Keyless OpenAI-compatible endpoint — no account or API token.
            url = "https://text.pollinations.ai/openai"
            payload = {
                "model": self.model_name,
                "messages": [
                    {"role": "system", "content": sys_prompt},
                    {"role": "user", "content": user_prompt}
                ],
                "temperature": 0.1
            }
            req = urllib.request.Request(
                url,
                data=json.dumps(payload).encode("utf-8"),
                headers={"Content-Type": "application/json"}
            )
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                res_data = json.loads(resp.read().decode("utf-8"))
                return res_data["choices"][0]["message"]["content"]

        elif self.provider == "ollama":
            url = f"{self.ollama_url}/api/generate"
            payload = {
                "model": self.model_name,
                "prompt": f"{sys_prompt}\n\n{user_prompt}",
                "stream": False
            }
            req = urllib.request.Request(url, data=json.dumps(payload).encode("utf-8"), headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                res_data = json.loads(resp.read().decode("utf-8"))
                return res_data.get("response", "")

        return "{}"

    def _extract_json(self, text: str) -> Dict[str, Any]:
        start = text.find("{")
        end = text.rfind("}")
        if start != -1 and end != -1:
            return json.loads(text[start : end + 1])
        return {}


class PlantDoer:
    """Deterministic, domain-grounded doer used as the LLM's offline fallback.

    Unlike :class:`MockDoer`, which is hard-wired to the Supervisor/Motor test
    schema, this maps the analytic diagnostic onto the gas plant's actuator
    interface: it only ever proposes a field from ``writable_actuators``.
    """

    def __init__(self, writable: Dict[str, Dict[str, Any]]):
        self.writable = writable

    def propose_action(
        self, semantic_state_str: str, anomaly_info: Dict[str, Any]
    ) -> ActionProposal:
        suspected = str(anomaly_info.get("suspected_fault", ""))
        attribution = anomaly_info.get("feature_attribution", {}) or {}
        top = max(attribution.items(), key=lambda kv: kv[1])[0] if attribution else ""
        dew = float(anomaly_info.get("dew_point_now_C", -105.0))
        spec = float(anomaly_info.get("dew_point_spec_C", -100.0))

        if dew > spec + 5.0 or "pressure" in suspected.lower() or "blower" in suspected.lower():
            return ActionProposal("SafetySystems", "esd_signal", True,
                                  f"Dew point {dew:.1f}C exceeds spec {spec:.0f}C; tripping ESD.")
        if "analyz" in suspected.lower() or "analys" in suspected.lower():
            return ActionProposal("OutletQuality", "analyzer_fault", True,
                                  "Analyser disagrees with bed behaviour; requesting calibration.")
        if "overload" in suspected.lower() or "flow" in top.lower():
            return ActionProposal("InletSeparation", "feed_flow.rate", 700.0,
                                  "Feed overload detected; derating feed to reduce bed loading.")
        return ActionProposal("AdsorberTowers", "lead_lag_index", 0,
                              f"Anomaly dominated by {top or 'tower loading'}; "
                              "forcing a regen switch to bring a fresher bed online.")
