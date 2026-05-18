#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "$#" -lt 1 ]; then
  echo "usage: $0 <camera_ip> [sound|path_vaddr] [volume] [telnet_port]" >&2
  echo "sounds: dingdong, factory, factory-en-96k, siren, hutong1, hutong2, tmp, tmp-wav" >&2
  echo "local MP3 paths must be V93-compatible unless PLAY_ALLOW_LOCAL_MP3=1 is set" >&2
  exit 2
fi

CAMERA_IP="$1"
SOUND="${2:-dingdong}"
VOLUME="${3:-10}"
TELNET_PORT="${4:-24}"
PLAY_WAIT_TIMEOUT="${PLAY_WAIT_TIMEOUT:-30}"
PLAY_CALL_MODE="${PLAY_CALL_MODE:-thread}"
PLAY_ALLOW_LOCAL_MP3="${PLAY_ALLOW_LOCAL_MP3:-0}"
PLAY_POST_SLEEP="${PLAY_POST_SLEEP:-}"
PLAY_STOP_DECODE="${PLAY_STOP_DECODE:-1}"
UPLOAD_FILE=""
REMOTE_UPLOAD_PATH="/tmp/speaker.mp3"
TARGET_ARG_FLAGS=""
LOCAL_MP3_COMPAT=0

case "$SOUND" in
  dingdong)
    TARGET_ARG_FLAGS="--arg0 0x0037b7b0"
    SOUND_NAME="dingdong"
    ;;
  factory|speaker-test|speaker_test)
    TARGET_ARG_FLAGS="--arg0 0x00378ec0"
    SOUND_NAME="factory"
    ;;
  factory-en-96k|factory_en_96k|en-hutong|en_hutong|highest-factory|highest_factory)
    TARGET_ARG_FLAGS="--arg0 0x003815d8"
    SOUND_NAME="factory-en-96k"
    ;;
  siren)
    TARGET_ARG_FLAGS="--arg0 0x0037e548"
    SOUND_NAME="siren"
    ;;
  hutong1|sound1)
    TARGET_ARG_FLAGS="--arg0 0x0037b7e4"
    SOUND_NAME="hutong1"
    ;;
  hutong2|sound2)
    TARGET_ARG_FLAGS="--arg0 0x00378d58"
    SOUND_NAME="hutong2"
    ;;
  tmp|speaker|speaker-mp3|speaker_mp3|tmp-speaker|tmp-mp3|tmp_mp3)
    TARGET_ARG_FLAGS="--arg0-string $REMOTE_UPLOAD_PATH"
    SOUND_NAME="tmp-speaker-mp3"
    ;;
  tmp-wav|tmp_wav|speaker-wav|speaker_wav)
    TARGET_ARG_FLAGS="--arg0 0x00378c84"
    SOUND_NAME="tmp-speaker"
    ;;
  0x*)
    TARGET_ARG_FLAGS="--arg0 $SOUND"
    SOUND_NAME="path-vaddr"
    ;;
  *)
    if [ -f "$SOUND" ]; then
      UPLOAD_FILE="$SOUND"
      TARGET_ARG_FLAGS="--arg0-string $REMOTE_UPLOAD_PATH"
      SOUND_NAME="local-mp3"
    else
      echo "unknown sound '$SOUND'; use dingdong, factory, factory-en-96k, siren, hutong1, hutong2, tmp, tmp-wav, a local MP3 path, or a 0x path vaddr" >&2
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

if [ -z "$PLAY_POST_SLEEP" ]; then
  PLAY_POST_SLEEP=6
  if [ -n "$UPLOAD_FILE" ] && command -v ffprobe >/dev/null 2>&1; then
    DURATION="$(ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 "$UPLOAD_FILE" 2>/dev/null || true)"
    case "$DURATION" in
      ''|*[!0-9.]*)
        ;;
      *)
        PLAY_POST_SLEEP="$(python3 - "$DURATION" <<'PY'
import math
import sys

print(max(6, int(math.ceil(float(sys.argv[1]) + 2.0))))
PY
)"
        ;;
    esac
  fi
fi

case "$PLAY_POST_SLEEP" in
  ''|*[!0-9]*)
    echo "PLAY_POST_SLEEP must be an integer" >&2
    exit 2
    ;;
esac

case "$PLAY_STOP_DECODE" in
  0|1)
    ;;
  *)
    echo "PLAY_STOP_DECODE must be 0 or 1" >&2
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

if [ -n "$UPLOAD_FILE" ] && command -v ffprobe >/dev/null 2>&1 && command -v od >/dev/null 2>&1; then
  MP3_CODEC="$(ffprobe -v error -select_streams a:0 -show_entries stream=codec_name -of default=nw=1:nk=1 "$UPLOAD_FILE" 2>/dev/null || true)"
  MP3_RATE="$(ffprobe -v error -select_streams a:0 -show_entries stream=sample_rate -of default=nw=1:nk=1 "$UPLOAD_FILE" 2>/dev/null || true)"
  MP3_CHANNELS="$(ffprobe -v error -select_streams a:0 -show_entries stream=channels -of default=nw=1:nk=1 "$UPLOAD_FILE" 2>/dev/null || true)"
  MP3_BITRATE="$(ffprobe -v error -select_streams a:0 -show_entries stream=bit_rate -of default=nw=1:nk=1 "$UPLOAD_FILE" 2>/dev/null || true)"
  MP3_HEADER="$(od -An -tx1 -N3 "$UPLOAD_FILE" 2>/dev/null | tr -d ' \n')"
  if [ "$MP3_CODEC" = "mp3" ] && \
     [ "$MP3_RATE" = "8000" ] && \
     [ "$MP3_CHANNELS" = "1" ] && \
     [ "$MP3_BITRATE" = "64000" ] && \
     [ "$MP3_HEADER" != "494433" ]; then
    LOCAL_MP3_COMPAT=1
  fi
fi

if [ -n "$UPLOAD_FILE" ] && [ "$PLAY_ALLOW_LOCAL_MP3" != "1" ] && [ "$LOCAL_MP3_COMPAT" != "1" ]; then
  echo "local MP3 is not in the validated V93 format and may restart anyka_ipc" >&2
  echo "encode it with scripts/encode_speaker_mp3_v93.sh or set PLAY_ALLOW_LOCAL_MP3=1 to force playback" >&2
  exit 2
fi

REMOTE_HELP="$(python3 "$REPO_ROOT/tools/telnet_exec.py" "$CAMERA_IP" \
  --port "$TELNET_PORT" \
  --wait 0.8 \
  --command 'if [ -x /tmp/rtsp_kick ] && /tmp/rtsp_kick --help 2>&1 | grep -q -- "--arg1" && /tmp/rtsp_kick --help 2>&1 | grep -q -- "--call-in-new-thread" && /tmp/rtsp_kick --help 2>&1 | grep -q -- "--arg0-string"; then echo rtsp_kick_speaker_ready; else echo rtsp_kick_needs_upload; fi' \
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
echo pid=\$PID sound=$SOUND_NAME target='$TARGET_ARG_FLAGS' volume=$VOLUME call_mode=$PLAY_CALL_MODE post_sleep=$PLAY_POST_SLEEP stop_decode=$PLAY_STOP_DECODE
/tmp/rtsp_kick \$PID --verbose --func-vaddr 0x0007c27c --guard-vaddr 0x0051ab34 --arg0 0 --arg1 2 --no-guard-check
sleep 1
/tmp/rtsp_kick \$PID --verbose --func-vaddr 0x0007cf9c --guard-vaddr 0x0051ab34 --arg0 $VOLUME --no-guard-check
sleep 1
/tmp/rtsp_kick \$PID --verbose --func-vaddr 0x0007c6c0 --guard-vaddr 0x0051ab34 $TARGET_ARG_FLAGS $PLAY_CALL_FLAGS --wait-timeout $PLAY_WAIT_TIMEOUT --no-guard-check
sleep $PLAY_POST_SLEEP
/tmp/rtsp_kick \$PID --verbose --func-vaddr 0x000cba60 --guard-vaddr 0x0051ab34 --arg0 0 --no-guard-check
if [ "$PLAY_STOP_DECODE" = "1" ]; then
  /tmp/rtsp_kick \$PID --verbose --func-vaddr 0x0007c590 --guard-vaddr 0x0051ab34 --arg0 0 --no-guard-check
fi
tail -120 /var/log/messages | grep -E 'ADEC|AO|playback|MP3|source|volume|send_stream|decode_count|get_frame_count' || true
"

python3 "$REPO_ROOT/tools/telnet_exec.py" "$CAMERA_IP" \
  --port "$TELNET_PORT" \
  --wait $((PLAY_WAIT_TIMEOUT + PLAY_POST_SLEEP + 10)).0 \
  --command "$REMOTE_COMMAND"
