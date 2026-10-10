#!/bin/bash
# run_experiment.sh - Start the full LLM-in-the-loop experiment
# Run from repository root: ./experiments/llm_in_loop/run_experiment.sh

set -e

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EXP_DIR="$ROOT_DIR/experiments/llm_in_loop"
BIN_DIR="$ROOT_DIR/.dist/linux-static-release"

echo "=================================================================="
echo "  SGRN LLM-in-the-loop Experiment Launcher"
echo "=================================================================="

# Check binaries
if [ ! -f "$BIN_DIR/gateway" ] || [ ! -f "$BIN_DIR/s7shell" ]; then
    echo "ERROR: Binaries not found. Build first with:"
    echo "  $ROOT_DIR/scripts/build.sh linux-static-release"
    exit 1
fi

# Check .env
if [ ! -f "$EXP_DIR/.env" ]; then
    echo "WARNING: .env not found. Copy .env.example and configure:"
    echo "  cp $EXP_DIR/.env.example $EXP_DIR/.env"
    echo "  # Then edit $EXP_DIR/.env with your LLM_API_KEY"
    exit 1
fi

# Load environment
source "$EXP_DIR/.env"

# Cleanup function
cleanup() {
    echo ""
    echo "Shutting down..."
    pkill -f "gateway.*llm_in_loop" 2>/dev/null || true
    pkill -f "s7shell.*simulation.as" 2>/dev/null || true
    pkill -f "llm_client.py" 2>/dev/null || true
    exit 0
}
trap cleanup INT TERM

# Start Gateway
echo ""
echo "Starting Gateway on port 8000..."
"$BIN_DIR/gateway" "$EXP_DIR/gateway.json" &
GATEWAY_PID=$!
sleep 2

# Check gateway started
if ! kill -0 $GATEWAY_PID 2>/dev/null; then
    echo "ERROR: Gateway failed to start"
    exit 1
fi
echo "Gateway started (PID: $GATEWAY_PID)"

# Start Simulation
echo ""
echo "Starting Soft PLC simulation..."
cd "$EXP_DIR"
"$BIN_DIR/s7shell" simulation.as &
SHELL_PID=$!
sleep 1

if ! kill -0 $SHELL_PID 2>/dev/null; then
    echo "ERROR: Simulation failed to start"
    kill $GATEWAY_PID 2>/dev/null
    exit 1
fi
echo "Simulation started (PID: $SHELL_PID)"

# Start LLM Client
echo ""
echo "Starting LLM Client..."
cd "$EXP_DIR"
python3 llm_client.py &
LLM_PID=$!
sleep 1

if ! kill -0 $LLM_PID 2>/dev/null; then
    echo "ERROR: LLM Client failed to start"
    kill $GATEWAY_PID $SHELL_PID 2>/dev/null
    exit 1
fi
echo "LLM Client started (PID: $LLM_PID)"

echo ""
echo "=================================================================="
echo "  Experiment Running!"
echo "=================================================================="
echo "  Dashboard: http://localhost:8000"
echo "  Gateway:   ws://localhost:8000/ws"
echo "  REST API:  http://localhost:8000/data/<DB_NAME>"
echo ""
echo "  Press Ctrl+C to stop all processes"
echo "=================================================================="

# Wait for any process to exit
wait -n $GATEWAY_PID $SHELL_PID $LLM_PID
EXIT_CODE=$?

echo ""
echo "One process exited. Shutting down..."
cleanup
exit $EXIT_CODE