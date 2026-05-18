#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
usage:
  record_template.sh rtsp CAMERA_IP OUTPUT.raw [seconds] [rate] [main|sub] [udp|tcp]
  record_template.sh host OUTPUT.raw [seconds] [rate] [alsa_device]

Examples:
  ./record_template.sh rtsp 192.168.1.130 templates/jarvis_01.raw 3 8000 sub udp
  ./record_template.sh host templates/jarvis_01.raw 3 8000 default

The output is signed 16-bit little-endian mono PCM with no WAV header.
USAGE
}

if [ "$#" -lt 1 ]; then
  usage
  exit 2
fi

mode="$1"
shift

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "ffmpeg is required on the host that records templates" >&2
  exit 2
fi

case "$mode" in
  rtsp)
    if [ "$#" -lt 2 ]; then
      usage
      exit 2
    fi
    camera_ip="$1"
    output="$2"
    seconds="${3:-3}"
    rate="${4:-8000}"
    stream="${5:-sub}"
    transport="${6:-udp}"
    case "$transport" in
      udp|tcp)
        ;;
      *)
        echo "invalid RTSP transport '$transport'; use udp or tcp" >&2
        exit 2
        ;;
    esac
    case "$stream" in
      main)
        rtsp_url="rtsp://$camera_ip:88/videoMain"
        ;;
      sub)
        rtsp_url="rtsp://$camera_ip:89/videoSub"
        ;;
      rtsp://*)
        rtsp_url="$stream"
        ;;
      *)
        echo "invalid RTSP stream '$stream'; use main, sub, or a full rtsp:// URL" >&2
        exit 2
        ;;
    esac
    mkdir -p "$(dirname "$output")"
    echo "Recording from camera RTSP microphone path: $rtsp_url transport=$transport" >&2
    echo "Say 'Jarvis' clearly near $camera_ip." >&2
    ffmpeg -hide_banner -y \
      -rtsp_transport "$transport" \
      -i "$rtsp_url" \
      -t "$seconds" \
      -vn \
      -ac 1 \
      -ar "$rate" \
      -acodec pcm_s16le \
      -f s16le \
      "$output"
    ;;
  host)
    if [ "$#" -lt 1 ]; then
      usage
      exit 2
    fi
    output="$1"
    seconds="${2:-3}"
    rate="${3:-8000}"
    device="${4:-default}"
    mkdir -p "$(dirname "$output")"
    echo "Recording from host ALSA device '$device'. Say 'Jarvis' clearly." >&2
    ffmpeg -hide_banner -y \
      -f alsa \
      -i "$device" \
      -t "$seconds" \
      -ac 1 \
      -ar "$rate" \
      -acodec pcm_s16le \
      -f s16le \
      "$output"
    ;;
  *)
    usage
    exit 2
    ;;
esac

bytes=$(wc -c < "$output")
echo "wrote $output ($bytes bytes)" >&2
