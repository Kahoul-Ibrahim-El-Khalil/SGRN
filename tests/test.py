#!/usr/bin/env python3
"""Deprecated shim: the test orchestrator moved to tests/run_tests.py."""
import os
import sys

print("tests/test.py is deprecated — forwarding to tests/run_tests.py", file=sys.stderr)
os.execv(sys.executable,
         [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "run_tests.py")] + sys.argv[1:])
