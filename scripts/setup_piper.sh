#!/usr/bin/env bash
# Vendor a SELF-CONTAINED Piper neural TTS (standalone binary + espeak data + a voice) into
# assets/piper/ so the game ships it and "just works" on install — NO Python, NO pip, NO
# first-run internet for players. Idempotent + best-effort; the game falls back to the OS
# voice if anything here is missing.
#
# For a Steam/distribution build: just include the whole assets/piper/ directory in the release.
# This script is the DEV-time provisioner that fills it in (run it once; commit/ship the result).
set -u
cd "$(dirname "$0")/.."

PIPER_DIR="assets/piper"
RUNTIME="$PIPER_DIR/runtime"           # holds the standalone binary + libs + espeak-ng-data
BIN="$RUNTIME/piper"
VOICE="$PIPER_DIR/voice.onnx"
mkdir -p "$PIPER_DIR"

REL="2023.11.14-2"   # rhasspy/piper standalone release (self-contained, no Python)
case "$(uname -s)-$(uname -m)" in
  Darwin-arm64)        ASSET="piper_macos_aarch64.tar.gz" ;;
  Darwin-x86_64)       ASSET="piper_macos_x64.tar.gz" ;;
  Linux-x86_64)        ASSET="piper_linux_x86_64.tar.gz" ;;
  Linux-aarch64)       ASSET="piper_linux_aarch64.tar.gz" ;;
  Linux-armv7l)        ASSET="piper_linux_armv7l.tar.gz" ;;
  *) echo "[piper] no standalone build for $(uname -s)-$(uname -m); leaving OS voice." >&2; ASSET="" ;;
esac

# 1) Standalone piper binary (self-contained: bundles its own libs + espeak-ng-data).
if [ -n "$ASSET" ] && [ ! -x "$BIN" ]; then
  echo "[piper] downloading standalone engine ($ASSET)..."
  TMP="$(mktemp -d)"
  if curl -fsSL "https://github.com/rhasspy/piper/releases/download/$REL/$ASSET" -o "$TMP/p.tgz" \
     && tar -xzf "$TMP/p.tgz" -C "$TMP"; then
    rm -rf "$RUNTIME"; mkdir -p "$RUNTIME"
    # the tarball extracts a top-level `piper/` dir — flatten it into $RUNTIME
    mv "$TMP/piper/"* "$RUNTIME/" 2>/dev/null || mv "$TMP"/*/* "$RUNTIME/" 2>/dev/null
    chmod +x "$BIN" 2>/dev/null || true
    xattr -dr com.apple.quarantine "$RUNTIME" 2>/dev/null || true   # macOS: clear Gatekeeper quarantine
  else
    echo "[piper] engine download failed (offline?)." >&2
  fi
  rm -rf "$TMP"
fi

# Verify the standalone engine actually RUNS (the macOS release historically ships without its
# dylibs). If it can't, drop it and fall back to a pip install of piper-tts (needs Python).
if [ -x "$BIN" ]; then
  if ! DYLD_LIBRARY_PATH="$RUNTIME" LD_LIBRARY_PATH="$RUNTIME" "$BIN" --help >/dev/null 2>&1; then
    echo "[piper] bundled engine can't run on this platform (missing libs) — falling back to pip." >&2
    rm -rf "$RUNTIME"
  fi
fi
if [ ! -x "$BIN" ]; then
  if ! command -v piper >/dev/null 2>&1 && [ ! -x "$HOME/.local/bin/piper" ]; then
    if command -v pipx >/dev/null 2>&1; then pipx install piper-tts >/dev/null 2>&1 && pipx ensurepath >/dev/null 2>&1 || true
    elif command -v pip3 >/dev/null 2>&1; then pip3 install --user piper-tts >/dev/null 2>&1 || true; fi
  fi
fi

# 2) A voice model (en_US-lessac-medium). Piper wants its config as <model>.onnx.json.
if [ ! -f "$VOICE" ] || [ ! -f "$VOICE.json" ]; then
  echo "[piper] downloading voice (en_GB-alan-medium — serious male, ~60MB)..."
  BASE="https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_GB/alan/medium/en_GB-alan-medium"
  if curl -fsSL "$BASE.onnx" -o "$VOICE.tmp" && curl -fsSL "$BASE.onnx.json" -o "$VOICE.json.tmp"; then
    mv "$VOICE.tmp" "$VOICE"; mv "$VOICE.json.tmp" "$VOICE.json"
  else
    rm -f "$VOICE.tmp" "$VOICE.json.tmp"
    echo "[piper] voice download failed (offline?). Using OS voice." >&2
  fi
fi

HAVE_PIPER=0
{ [ -x "$BIN" ] || command -v piper >/dev/null 2>&1 || [ -x "$HOME/.local/bin/piper" ]; } && HAVE_PIPER=1
if [ "$HAVE_PIPER" = 1 ] && [ -f "$VOICE" ]; then
  if [ -x "$BIN" ]; then echo "[piper] ready — SELF-CONTAINED engine vendored at $PIPER_DIR (ship this dir; zero player setup)."
  else echo "[piper] ready — using the pip/system piper engine + vendored voice (this platform's standalone build is unavailable)."; fi
else
  echo "[piper] not fully provisioned; the game will use the OS voice until it is."
fi
