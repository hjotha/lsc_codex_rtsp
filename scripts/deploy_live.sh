#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "$#" -lt 1 ]; then
  echo "usage: $0 <camera_ip> [telnet_port]" >&2
  exit 2
fi

CAMERA_IP="$1"
TELNET_PORT="${2:-24}"

bash "$REPO_ROOT/scripts/build_anyka.sh" >/dev/null
bash "$REPO_ROOT/scripts/make_deploy_reader8554_telnet.sh" >/dev/null

python3 "$REPO_ROOT/tools/telnet_exec.py" "$CAMERA_IP" \
  --port "$TELNET_PORT" \
  --wait 15 \
  --file "$REPO_ROOT/out/deploy_reader8554_to_sd.telnet"
