#!/usr/bin/env bash
# Auto-provision Piper neural TTS so in-game taunts use a neural voice instead of the OS voice.
# Idempotent + best-effort: installs the `piper` CLI (pipx/pip) and downloads a voice model to
# assets/piper/voice.onnx (which the game auto-detects). Safe to run on every build/launch.
set -u
cd "$(dirname "$0")/.."

VOICE_DIR="assets/piper"
VOICE="$VOICE_DIR/voice.onnx"
mkdir -p "$VOICE_DIR"

# Already fully set up? Nothing to do.
if [ -f "$VOICE" ] && [ -f "$VOICE.json" ] && { command -v piper >/dev/null 2>&1 || [ -x "$HOME/.local/bin/piper" ]; }; then
  echo "[piper] already set up ($VOICE)"; exit 0
fi

# 1) Install the piper CLI if it's missing.
if ! command -v piper >/dev/null 2>&1 && [ ! -x "$HOME/.local/bin/piper" ]; then
  echo "[piper] installing the piper-tts CLI..."
  if command -v pipx >/dev/null 2>&1; then
    pipx install piper-tts >/dev/null 2>&1 || pip3 install --user piper-tts >/dev/null 2>&1
    pipx ensurepath >/dev/null 2>&1 || true
  elif command -v pip3 >/dev/null 2>&1; then
    pip3 install --user piper-tts >/dev/null 2>&1
  else
    echo "[piper] no pipx/pip3 found — install Python 3 + pip, then re-run this script." >&2
  fi
fi

# 2) Download a voice (en_US-lessac-medium) if missing. Piper expects the config next to the
#    model as <model>.onnx.json, so we save voice.onnx + voice.onnx.json.
if [ ! -f "$VOICE" ] || [ ! -f "$VOICE.json" ]; then
  echo "[piper] downloading voice (en_US-lessac-medium, ~60MB)..."
  BASE="https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium"
  if curl -fsSL "$BASE.onnx" -o "$VOICE.tmp" && curl -fsSL "$BASE.onnx.json" -o "$VOICE.json.tmp"; then
    mv "$VOICE.tmp" "$VOICE"; mv "$VOICE.json.tmp" "$VOICE.json"
  else
    rm -f "$VOICE.tmp" "$VOICE.json.tmp"
    echo "[piper] voice download failed (offline?). The game will use the OS voice until this succeeds." >&2
    exit 0
  fi
fi

if command -v piper >/dev/null 2>&1 || [ -x "$HOME/.local/bin/piper" ]; then
  echo "[piper] ready — neural voice at $VOICE"
else
  echo "[piper] voice ready, but the 'piper' CLI isn't on PATH. Set DUNGEON_PIPER_BIN to its path, or add ~/.local/bin to PATH."
fi
