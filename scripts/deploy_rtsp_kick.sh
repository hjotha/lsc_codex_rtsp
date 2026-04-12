#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "$#" -lt 1 ]; then
  echo "usage: $0 <camera_ip> [telnet_port]" >&2
  exit 2
fi

CAMERA_IP="$1"
TELNET_PORT="${2:-24}"

bash "$REPO_ROOT/scripts/build_rtsp_kick_anyka.sh" >/dev/null

python3 "$REPO_ROOT/tools/telnet_upload_file.py" "$CAMERA_IP" \
  "$REPO_ROOT/out/rtsp_kick_arm" \
  /tmp/rtsp_kick \
  --port "$TELNET_PORT" \
  --wait 0.8 \
  --chunk-lines 4 \
  --commands-per-session 1
