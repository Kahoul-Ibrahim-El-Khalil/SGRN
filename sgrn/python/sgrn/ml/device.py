#!/usr/bin/env python3
"""
sgrn.ml.device — Hardware Acceleration & Compute Device Discovery.

Supports:
  - AMD ROCm / HIP GPUs
  - NVIDIA CUDA GPUs
  - Apple Silicon Metal Performance Shaders (MPS)
  - Host CPU Fallback
"""

from __future__ import annotations

from typing import Any, Dict, Optional, Union

try:
    import torch
    HAS_TORCH = True
except ImportError:
    HAS_TORCH = False


def getHardwareInfo() -> Dict[str, Any]:
    """Inspect and return available hardware acceleration platform and vendor metadata.
    
    Detects:
      - AMD ROCm / HIP platforms
      - NVIDIA CUDA platforms
      - Apple Silicon (MPS)
      - CPU host execution
    """
    info: Dict[str, Any] = {
        "platform": "CPU",
        "deviceType": "cpu",
        "deviceCount": 0,
        "deviceName": "CPU",
        "backend": "host",
        "isAccelerated": False,
    }
    if not HAS_TORCH:
        return info

    # 1. AMD ROCm / HIP Detection
    if hasattr(torch.version, "hip") and torch.version.hip is not None:
        count = torch.cuda.device_count() if torch.cuda.is_available() else 0
        info["platform"] = "AMD ROCm"
        info["backend"] = f"HIP {torch.version.hip}"
        info["deviceType"] = "cuda"
        info["deviceCount"] = count
        info["isAccelerated"] = count > 0
        if count > 0:
            info["deviceName"] = torch.cuda.get_device_name(0)
        return info

    # 2. NVIDIA CUDA Detection
    if torch.cuda.is_available():
        count = torch.cuda.device_count()
        info["platform"] = "NVIDIA CUDA"
        info["backend"] = f"CUDA {getattr(torch.version, 'cuda', 'unknown')}"
        info["deviceType"] = "cuda"
        info["deviceCount"] = count
        info["isAccelerated"] = True
        if count > 0:
            info["deviceName"] = torch.cuda.get_device_name(0)
        return info

    # 3. Apple Silicon MPS Detection
    if hasattr(torch.backends, "mps") and torch.backends.mps.is_available():
        info["platform"] = "Apple Silicon"
        info["backend"] = "Metal MPS"
        info["deviceType"] = "mps"
        info["deviceCount"] = 1
        info["deviceName"] = "Apple GPU"
        info["isAccelerated"] = True
        return info

    return info


def resolveDevice(device: Optional[Union[str, Any]] = None) -> Any:
    """Resolve compute device with automatic NVIDIA CUDA, AMD ROCm, and Apple MPS discovery.
    
    Args:
        device: Explicit device string ('cuda', 'cuda:0', 'mps', 'cpu') or torch.device.
                If None, auto-selects the fastest available hardware accelerator.
    """
    if not HAS_TORCH:
        raise ImportError("PyTorch is required for device resolution.")

    if device is not None:
        if isinstance(device, str):
            return torch.device(device)
        return device

    hw = getHardwareInfo()
    if hw["isAccelerated"]:
        return torch.device(hw["deviceType"])
    return torch.device("cpu")


# Backward-compatible snake_case aliases
get_hardware_info = getHardwareInfo
resolve_device = resolveDevice
