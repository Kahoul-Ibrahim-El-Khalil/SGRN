#!/usr/bin/env python3
"""Motor starting simulation - generates telemetry for training and testing."""

from __future__ import annotations

import argparse
import csv
import json
import math
import random
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import List, Dict, Any

ROOT = Path(__file__).resolve().parents[2]
CONFIG_PATH = Path(__file__).with_name("config.yaml")


@dataclass
class MotorParams:
    rated_voltage: float = 480.0
    rated_current: float = 10.0
    rated_speed: float = 1800.0
    poles: int = 4
    frequency: float = 60.0
    inertia: float = 0.1
    load_torque: float = 5.0
    inrush_multiplier: float = 6.0


@dataclass
class SimConfig:
    duration_s: float = 30.0
    sample_rate_hz: float = 100.0
    dt: float = 0.01
    normal_starts: int = 20
    fault_starts: int = 5
    fault_types: List[str] = None


class MotorSimulator:
    def __init__(self, motor: MotorParams, cfg: SimConfig):
        self.motor = motor
        self.cfg = cfg

    def _calculateSlip(self, speed_rpm: float) -> float:
        sync_rpm = 120 * self.motor.frequency / self.motor.poles
        if sync_rpm == 0:
            return 0.0
        return max(0.0, (sync_rpm - speed_rpm) / sync_rpm)

    def _calculateCurrent(self, slip: float, imbalance: float = 0.0) -> float:
        s_rated = 0.03
        base_current = self.motor.rated_current / math.sqrt(1 + (slip / s_rated) ** 2)
        inrush = self.motor.inrush_multiplier * self.motor.rated_current * math.exp(-50 * slip)
        current = base_current + inrush
        if imbalance > 0:
            current *= (1 + imbalance)
        return current

    def _calculateTorque(self, slip: float) -> float:
        s_rated = 0.03
        t_max = 2 * self.motor.load_torque
        torque = 2 * t_max * slip * s_rated / (slip**2 + s_rated**2)
        return torque

    def simulateStart(self, fault_type: str = "none", imbalance: float = 0.0, seed: int = 0) -> List[Dict[str, Any]]:
        random.seed(seed)
        samples = []
        speed = 0.0
        n_samples = int(self.cfg.duration_s / self.cfg.dt)

        for i in range(n_samples):
            t = i * self.cfg.dt

            if fault_type == "stall":
                target_speed = 0.0
                alpha = 0.0
            elif fault_type == "overcurrent":
                target_speed = self.motor.rated_speed * 0.95
                alpha = 0.3
            elif fault_type == "bearing_fault":
                target_speed = self.motor.rated_speed * 0.98
                alpha = 0.4
            else:
                target_speed = self.motor.rated_speed
                alpha = 0.5

            speed += alpha * (target_speed - speed) * self.cfg.dt * 10
            speed += random.gauss(0, 1.0)

            slip = self._calculateSlip(speed)
            current = self._calculateCurrent(slip, imbalance if fault_type == "phase_imbalance" else 0.0)
            torque = self._calculateTorque(slip)

            if fault_type == "overcurrent" and t > 5.0:
                current *= 1.6

            vibration = abs(random.gauss(0, 0.1))
            if fault_type == "bearing_fault":
                vibration += 2.0 + 0.5 * math.sin(2 * math.pi * 60 * t)

            voltage = self.motor.rated_voltage * (1 + random.gauss(0, 0.01))
            power = voltage * current * math.sqrt(3) / 1000

            samples.append({
                "time": round(t, 4),
                "voltage": round(voltage, 2),
                "current": round(current, 3),
                "speed": round(speed, 1),
                "torque": round(torque, 3),
                "slip": round(slip, 4),
                "power": round(power, 3),
                "vibration": round(vibration, 3),
            })

        return samples


def generateDataset(output_dir: Path, config: Dict[str, Any]) -> None:
    motor = MotorParams(**config["motor"])
    sim_cfg = SimConfig(
        duration_s=config["simulation"]["duration_s"],
        sample_rate_hz=config["simulation"]["sample_rate_hz"],
        dt=config["simulation"]["dt"],
        normal_starts=config["scenario"]["normal_starts"],
        fault_starts=config["scenario"]["fault_starts"],
        fault_types=config["scenario"]["fault_types"],
    )
    simulator = MotorSimulator(motor, sim_cfg)

    nominal_data = []
    for i in range(sim_cfg.normal_starts):
        samples = simulator.simulateStart(fault_type="none", seed=i)
        nominal_data.extend(samples)

    faulty_data = []
    fault_types = sim_cfg.fault_types
    for i in range(sim_cfg.fault_starts):
        fault_type = fault_types[i % len(fault_types)]
        samples = simulator.simulateStart(fault_type=fault_type, seed=1000 + i)
        faulty_data.extend(samples)

    output_dir.mkdir(parents=True, exist_ok=True)
    features = config["features"]

    def writeCsv(data: List[Dict], path: Path):
        fieldnames = ["time"] + features
        with open(path, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(data)

    writeCsv(nominal_data, output_dir / "nominal_starts.csv")
    writeCsv(faulty_data, output_dir / "faulty_starts.csv")

    meta = {
        "motor": asdict(motor),
        "config": config,
        "nominal_samples": len(nominal_data),
        "faulty_samples": len(faulty_data),
    }
    (output_dir / "meta.json").write_text(json.dumps(meta, indent=2))

    print(f"[simulation] Generated {len(nominal_data)} nominal samples")
    print(f"[simulation] Generated {len(faulty_data)} faulty samples")
    print(f"[simulation] Saved to {output_dir}")


def main() -> int:
    import yaml

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", default=str(CONFIG_PATH), help="Config file path")
    parser.add_argument("--output-dir", default=str(Path(__file__).parent), help="Output directory")
    args = parser.parse_args()

    config_path = Path(args.config)
    if config_path.suffix in (".yaml", ".yml"):
        config = yaml.safe_load(config_path.read_text())
    else:
        config = json.loads(config_path.read_text())

    output_dir = Path(args.output_dir)
    generateDataset(output_dir, config)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())