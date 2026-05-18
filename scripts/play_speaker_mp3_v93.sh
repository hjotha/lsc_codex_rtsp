#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "$#" -lt 1 ]; then
  echo "usage: $0 <camera_ip> [sound|path_vaddr] [volume] [telnet_port]" >&2
  echo "sounds: dingdong, factory, factory-en-96k, siren, hutong1, hutong2, tmp" >&2
  echo "local MP3 paths are experimental; set PLAY_ALLOW_LOCAL_MP3=1 to enable" >&2
  exit 2
fi

CAMERA_IP="$1"
SOUND="${2:-dingdong}"
VOLUME="${3:-10}"
TELNET_PORT="${4:-24}"
PLAY_WAIT_TIMEOUT="${PLAY_WAIT_TIMEOUT:-30}"
PLAY_CALL_MODE="${PLAY_CALL_MODE:-thread}"
PLAY_ALLOW_LOCAL_MP3="${PLAY_ALLOW_LOCAL_MP3:-0}"
UPLOAD_FILE=""
REMOTE_UPLOAD_PATH="/tmp/speaker.wav"

case "$SOUND" in
  dingdong)
    PATH_VADDR="0x0037b7b0"
    SOUND_NAME="dingdong"
    ;;
  factory|speaker-test|speaker_test)
    PATH_VADDR="0x00378ec0"
    SOUND_NAME="factory"
    ;;
  factory-en-96k|factory_en_96k|en-hutong|en_hutong|highest-factory|highest_factory)
    PATH_VADDR="0x003815d8"
    SOUND_NAME="factory-en-96k"
    ;;
  siren)
    PATH_VADDR="0x0037e548"
    SOUND_NAME="siren"
    ;;
  hutong1|sound1)
    PATH_VADDR="0x0037b7e4"
    SOUND_NAME="hutong1"
    ;;
  hutong2|sound2)
    PATH_VADDR="0x00378d58"
    SOUND_NAME="hutong2"
    ;;
  tmp|speaker|speaker-wav|speaker_wav|tmp-speaker)
    PATH_VADDR="0x00378c84"
    SOUND_NAME="tmp-speaker"
    ;;
  0x*)
    PATH_VADDR="$SOUND"
    SOUND_NAME="path-vaddr"
    ;;
  *)
    if [ -f "$SOUND" ]; then
      UPLOAD_FILE="$SOUND"
      PATH_VADDR="0x00378c84"
      SOUND_NAME="local-mp3"
    else
      echo "unknown sound '$SOUND'; use dingdong, factory, factory-en-96k, siren, hutong1, hutong2, tmp, a local MP3 path, or a 0x path vaddr" >&2
      exit 2
    fi
    ;;
esac

case "$VOLUME" in
  ''|*[!0-9]*)
    echo "volume must be an integer" >&2
    exit 2
    ;;
esac

case "$PLAY_WAIT_TIMEOUT" in
  ''|*[!0-9]*)
    echo "PLAY_WAIT_TIMEOUT must be an integer" >&2
    exit 2
    ;;
esac

case "$PLAY_CALL_MODE" in
  thread)
    PLAY_CALL_FLAGS="--call-in-new-thread --malloc-vaddr 0x000607b4 --thread-create-vaddr 0x0012208c --thread-stack-size 0x10000"
    ;;
  direct)
    PLAY_CALL_FLAGS=""
    ;;
  *)
    echo "PLAY_CALL_MODE must be 'thread' or 'direct'" >&2
    exit 2
    ;;
esac

if [ -n "$UPLOAD_FILE" ] && [ "$PLAY_ALLOW_LOCAL_MP3" != "1" ]; then
  echo "local MP3 playback is experimental and restarted anyka_ipc in live testing" >&2
  echo "set PLAY_ALLOW_LOCAL_MP3=1 to upload and play a local file anyway" >&2
  exit 2
fi

REMOTE_HELP="$(python3 "$REPO_ROOT/tools/telnet_exec.py" "$CAMERA_IP" \
  --port "$TELNET_PORT" \
  --wait 0.8 \
  --command 'if [ -x /tmp/rtsp_kick ] && /tmp/rtsp_kick --help 2>&1 | grep -q -- "--arg1" && /tmp/rtsp_kick --help 2>&1 | grep -q -- "--call-in-new-thread"; then echo rtsp_kick_speaker_ready; else echo rtsp_kick_needs_upload; fi' \
  2>&1 || true)"

if ! printf '%s\n' "$REMOTE_HELP" | grep -q 'rtsp_kick_speaker_ready'; then
  bash "$REPO_ROOT/scripts/deploy_rtsp_kick.sh" "$CAMERA_IP" "$TELNET_PORT" >/dev/null
fi

if [ -n "$UPLOAD_FILE" ]; then
  UPLOADED_MP3=0
  if command -v nc >/dev/null 2>&1; then
    if python3 "$REPO_ROOT/tools/telnet_upload_nc.py" "$CAMERA_IP" "$UPLOAD_FILE" "$REMOTE_UPLOAD_PATH" \
      --port "$TELNET_PORT" \
      --wait 0.8 \
      --mode 644 >/dev/null; then
      UPLOADED_MP3=1
    fi
  fi
  if [ "$UPLOADED_MP3" -eq 0 ]; then
    python3 "$REPO_ROOT/tools/telnet_upload_file.py" "$CAMERA_IP" "$UPLOAD_FILE" "$REMOTE_UPLOAD_PATH" \
      --port "$TELNET_PORT" \
      --mode 644 >/dev/null
  fi
fi

REMOTE_COMMAND="
PID=\$(pidof anyka_ipc | awk '{print \$1}')
if [ -z \"\$PID\" ]; then
  echo 'anyka_ipc not running'
  exit 1
fi
echo pid=\$PID sound=$SOUND_NAME path_vaddr=$PATH_VADDR volume=$VOLUME call_mode=$PLAY_CALL_MODE
/tmp/rtsp_kick \$PID --verbose --func-vaddr 0x0007c27c --guard-vaddr 0x0051ab34 --arg0 0 --arg1 2 --no-guard-check
sleep 1
/tmp/rtsp_kick \$PID --verbose --func-vaddr 0x0007cf9c --guard-vaddr 0x0051ab34 --arg0 $VOLUME --no-guard-check
sleep 1
/tmp/rtsp_kick \$PID --verbose --func-vaddr 0x0007c6c0 --guard-vaddr 0x0051ab34 --arg0 $PATH_VADDR $PLAY_CALL_FLAGS --wait-timeout $PLAY_WAIT_TIMEOUT --no-guard-check
sleep 6
/tmp/rtsp_kick \$PID --verbose --func-vaddr 0x000cba60 --guard-vaddr 0x0051ab34 --arg0 0 --no-guard-check
tail -120 /var/log/messages | grep -E 'ADEC|AO|playback|MP3|source|volume|send_stream|decode_count|get_frame_count' || true
"

python3 "$REPO_ROOT/tools/telnet_exec.py" "$CAMERA_IP" \
  --port "$TELNET_PORT" \
  --wait $((PLAY_WAIT_TIMEOUT + 10)).0 \
  --command "$REMOTE_COMMAND"
