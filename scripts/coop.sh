#!/usr/bin/env bash
# Launch a local co-op session: 1 host + (N-1) clients, all on loopback.
# Usage: ./scripts/coop.sh [N]   (default N=2).  Ctrl-C tears the whole lot down.
set -euo pipefail
cd "$(dirname "$0")/.."

N="${1:-2}"
if ! [[ "$N" =~ ^[0-9]+$ ]] || [ "$N" -lt 1 ]; then
    echo "usage: $0 N   (N = total windows, >= 1)" >&2
    exit 1
fi
PORT="${PORT:-7777}"

./scripts/build.sh

pids=()
cleanup() { kill "${pids[@]}" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

echo "[coop] host on port $PORT"
./build/dungeoncrawl --host "$PORT" &
pids+=("$!")

sleep 0.5   # let the host bind before clients dial in

for ((i = 1; i < N; ++i)); do
    echo "[coop] client $i -> 127.0.0.1:$PORT"
    ./build/dungeoncrawl --connect 127.0.0.1 "$PORT" &
    pids+=("$!")
    sleep 0.3
done

echo "[coop] $N window(s) up (1 host, $((N - 1)) client(s)). Ctrl-C to stop all."
wait
