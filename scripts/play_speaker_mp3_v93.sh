#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "$#" -lt 1 ]; then
  echo "usage: $0 <camera_ip> [sound|path_vaddr] [volume] [telnet_port]" >&2
  echo "sounds: dingdong, factory, siren, hutong1, hutong2" >&2
  exit 2
fi

CAMERA_IP="$1"
SOUND="${2:-dingdong}"
VOLUME="${3:-10}"
TELNET_PORT="${4:-24}"

case "$SOUND" in
  dingdong)
    PATH_VADDR="0x0037b7b0"
    ;;
  factory|speaker-test|speaker_test)
    PATH_VADDR="0x00378ec0"
    ;;
  siren)
    PATH_VADDR="0x0037e548"
    ;;
  hutong1|sound1)
    PATH_VADDR="0x0037b7e4"
    ;;
  hutong2|sound2)
    PATH_VADDR="0x00378d58"
    ;;
  0x*)
    PATH_VADDR="$SOUND"
    ;;
  *)
    echo "unknown sound '$SOUND'; use dingdong, factory, siren, hutong1, hutong2, or a 0x path vaddr" >&2
    exit 2
    ;;
esac

case "$VOLUME" in
  ''|*[!0-9]*)
    echo "volume must be an integer" >&2
    exit 2
    ;;
esac

REMOTE_HELP="$(python3 "$REPO_ROOT/tools/telnet_exec.py" "$CAMERA_IP" \
  --port "$TELNET_PORT" \
  --wait 0.8 \
  --command 'if [ -x /tmp/rtsp_kick ] && /tmp/rtsp_kick --help 2>&1 | grep -q -- "--arg1"; then echo rtsp_kick_arg1_ready; else echo rtsp_kick_needs_upload; fi' \
  2>&1 || true)"

if ! printf '%s\n' "$REMOTE_HELP" | grep -q 'rtsp_kick_arg1_ready'; then
  bash "$REPO_ROOT/scripts/deploy_rtsp_kick.sh" "$CAMERA_IP" "$TELNET_PORT" >/dev/null
fi

REMOTE_COMMAND="
PID=\$(pidof anyka_ipc | awk '{print \$1}')
if [ -z \"\$PID\" ]; then
  echo 'anyka_ipc not running'
  exit 1
fi
echo pid=\$PID sound=$SOUND path_vaddr=$PATH_VADDR volume=$VOLUME
/tmp/rtsp_kick \$PID --verbose --func-vaddr 0x0007c27c --guard-vaddr 0x0051ab34 --arg0 0 --arg1 2 --no-guard-check
sleep 1
/tmp/rtsp_kick \$PID --verbose --func-vaddr 0x0007cf9c --guard-vaddr 0x0051ab34 --arg0 $VOLUME --no-guard-check
sleep 1
/tmp/rtsp_kick \$PID --verbose --func-vaddr 0x0007c6c0 --guard-vaddr 0x0051ab34 --arg0 $PATH_VADDR --no-guard-check
sleep 2
/tmp/rtsp_kick \$PID --verbose --func-vaddr 0x000cba60 --guard-vaddr 0x0051ab34 --arg0 0 --no-guard-check
tail -120 /var/log/messages | grep -E 'ADEC|AO|playback|MP3|source|volume|send_stream|decode_count|get_frame_count' || true
"

python3 "$REPO_ROOT/tools/telnet_exec.py" "$CAMERA_IP" \
  --port "$TELNET_PORT" \
  --wait 8.0 \
  --command "$REMOTE_COMMAND"
