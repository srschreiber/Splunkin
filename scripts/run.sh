#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
./scripts/build.sh
# Auto-provision Piper neural TTS in the background (idempotent; first run downloads a voice,
# later runs return instantly). The game falls back to the OS voice until it's ready.
( ./scripts/setup_piper.sh >/dev/null 2>&1 & ) || true
exec ./build/dungeoncrawl ${ARGS:-}
