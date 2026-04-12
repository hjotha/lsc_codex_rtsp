#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$REPO_ROOT/out/anyka_ring_rtsp_server_arm"
OUT="$REPO_ROOT/out/deploy_reader8554_to_sd.telnet"

if [ ! -f "$BIN" ]; then
  echo "Missing $BIN. Run bash scripts/build_anyka.sh first." >&2
  exit 2
fi

mkdir -p "$REPO_ROOT/out"

{
  echo "killall anyka_ring_rtsp_server >/dev/null 2>&1 || true"
  echo "cat >/tmp/sd/anyka_ring_rtsp_server.b64 <<'EOF'"
  base64 "$BIN"
  echo "EOF"
  echo "base64 -d /tmp/sd/anyka_ring_rtsp_server.b64 >/tmp/sd/anyka_ring_rtsp_server"
  echo "chmod +x /tmp/sd/anyka_ring_rtsp_server"
  echo "rm -f /tmp/sd/anyka_ring_rtsp_server.b64"
  echo "/tmp/sd/anyka_ring_rtsp_server --ring /tmp/VideoMainStream0 --port 8554 --loop-forever --verbose >/tmp/sd/reader8554.log 2>&1 &"
  echo "sleep 1"
  echo "netstat -lnpt 2>/dev/null || netstat -lnp 2>/dev/null"
} >"$OUT"

wc -l "$OUT"
ls -lh "$OUT"
