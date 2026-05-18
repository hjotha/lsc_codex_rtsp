#!/usr/bin/env bash
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${JARVIS_WAKE_BIN:-$DIR/jarvis_wake}"
TEMPLATE="${JARVIS_WAKE_TEMPLATE:-$DIR/templates/jarvis_01.raw}"
RATE="${JARVIS_WAKE_RATE:-8000}"
HOST="${JARVIS_WAKE_HOST:-192.168.1.70}"
PORT="${JARVIS_WAKE_PORT:-18070}"
PATH_NAME="${JARVIS_WAKE_PATH:-/v1/wake}"

if [ ! -x "$BIN" ]; then
  echo "missing binary: $BIN" >&2
  echo "build with: make -C $DIR" >&2
  exit 2
fi

if [ ! -f "$TEMPLATE" ]; then
  echo "missing template: $TEMPLATE" >&2
  echo "record one with: $DIR/record_template.sh rtsp CAMERA_IP $TEMPLATE" >&2
  exit 2
fi

exec "$BIN" \
  --stdin \
  --rate "$RATE" \
  --template "$TEMPLATE" \
  --host "$HOST" \
  --port "$PORT" \
  --path "$PATH_NAME" \
  "$@"
