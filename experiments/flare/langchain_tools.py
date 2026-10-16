#!/usr/bin/env python3
"""Read-only LangChain tools for the SGRN Gas Processing Flare Experiment.

Tools exposed:
  - get_latest_detector_state(): returns score, state, thresholds, top signals with context
  - get_signal_trend(signal, n): returns recent n-step history and rate of change
  - get_replay_status(): returns current simulation time, speed, and frame count
"""

from __future__ import annotations

import json
from typing import Any, Dict, List, Optional, Type

from langchain_core.callbacks import CallbackManagerForToolRun
from langchain_core.tools import BaseTool
from pydantic import BaseModel, Field


class DetectorStateStore:
    """Thread-safe state store shared between live detector polling loop and agent tools."""

    def __init__(self):
        self.latest_state: Dict[str, Any] = {
            "sim_time": "N/A",
            "state": "normal",
            "score": 0.0,
            "q_alpha": 0.0,
            "q_upper": 0.0,
            "top_signals": [],
        }
        self.signal_history: Dict[str, List[float]] = {}
        self.history_timestamps: List[str] = []
        self.replay_status: Dict[str, Any] = {
            "sim_time": "N/A",
            "speed": 1.0,
            "frames": 0,
            "running": False,
        }

    def update(
        self,
        detector_result: Dict[str, Any],
        sim_time: str,
        current_signals: Dict[str, float],
        replay_info: Dict[str, Any],
    ) -> None:
        self.latest_state = {
            "sim_time": sim_time,
            "state": detector_result.get("state", "normal"),
            "score": float(detector_result.get("score", 0.0)),
            "q_alpha": float(detector_result.get("q_alpha", 0.0)),
            "q_upper": float(detector_result.get("q_upper", 0.0)),
            "top_signals": detector_result.get("top_signals", []),
        }
        self.history_timestamps.append(sim_time)
        if len(self.history_timestamps) > 100:
            self.history_timestamps.pop(0)

        for sig, val in current_signals.items():
            if sig not in self.signal_history:
                self.signal_history[sig] = []
            self.signal_history[sig].append(float(val))
            if len(self.signal_history[sig]) > 100:
                self.signal_history[sig].pop(0)

        self.replay_status = dict(replay_info)


# Global instance for tool binding
GLOBAL_STORE = DetectorStateStore()


class NoArgsInput(BaseModel):
    pass


class SignalTrendInput(BaseModel):
    signal: str = Field(description="Signal path name, e.g. 'SafetySystems.flare_header_press' or 'InletSeparation.feed_pressure'")
    n: int = Field(default=5, description="Number of recent readings to inspect")


class GetLatestDetectorStateTool(BaseTool):
    name: str = "get_latest_detector_state"
    description: str = (
        "Get the most recent conformal anomaly detector state. "
        "Returns JSON containing simulation time, operational state (normal, uncertain, or abnormal), "
        "reconstruction score vs conformal threshold q_alpha and upper threshold q_upper, and top-3 signals "
        "with their current value and normal baseline range."
    )
    args_schema: Type[BaseModel] = NoArgsInput

    def _run(self, run_manager: Optional[CallbackManagerForToolRun] = None) -> str:
        return json.dumps(GLOBAL_STORE.latest_state, indent=2)


class GetSignalTrendTool(BaseTool):
    name: str = "get_signal_trend"
    description: str = (
        "Get the recent numerical trend and rate of change for a specific plant signal over the last n readings. "
        "Helps assess whether pressure or temperature is climbing progressively or fluctuating normally."
    )
    args_schema: Type[BaseModel] = SignalTrendInput

    def _run(self, signal: str, n: int = 5, run_manager: Optional[CallbackManagerForToolRun] = None) -> str:
        hist = GLOBAL_STORE.signal_history.get(signal, [])
        if not hist:
            # Check for partial match
            matches = [k for k in GLOBAL_STORE.signal_history.keys() if signal in k]
            if matches:
                signal = matches[0]
                hist = GLOBAL_STORE.signal_history[signal]
            else:
                return json.dumps({"error": f"Signal '{signal}' not found in telemetry history."})

        recent = hist[-max(1, n) :]
        delta = recent[-1] - recent[0] if len(recent) > 1 else 0.0
        return json.dumps({
            "signal": signal,
            "recent_values": recent,
            "min": min(recent),
            "max": max(recent),
            "net_change": delta,
            "rate_per_sample": delta / max(1, len(recent) - 1),
        }, indent=2)


class GetReplayStatusTool(BaseTool):
    name: str = "get_replay_status"
    description: str = "Get current digital twin replay status: simulation timestamp, playback speed, and frame count."
    args_schema: Type[BaseModel] = NoArgsInput

    def _run(self, run_manager: Optional[CallbackManagerForToolRun] = None) -> str:
        return json.dumps(GLOBAL_STORE.replay_status, indent=2)


def getFlareTools() -> List[BaseTool]:
    """Return read-only tools for the flare monitoring agent."""
    return [
        GetLatestDetectorStateTool(),
        GetSignalTrendTool(),
        GetReplayStatusTool(),
    ]
