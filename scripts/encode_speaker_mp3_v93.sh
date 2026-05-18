#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
  echo "usage: $0 <input-audio> <output.mp3>" >&2
  exit 2
fi

INPUT_AUDIO="$1"
OUTPUT_MP3="$2"

if [ ! -f "$INPUT_AUDIO" ]; then
  echo "input file not found: $INPUT_AUDIO" >&2
  exit 2
fi

for tool in ffmpeg ffprobe; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "missing required tool: $tool" >&2
    exit 2
  fi
done

mkdir -p "$(dirname "$OUTPUT_MP3")"

# Match the proven generated format for V3.2863.93 speaker playback:
# MPEG Layer III v2.5, 8000 Hz, mono, 64 kbps, no ID3/Xing metadata.
ffmpeg -hide_banner -loglevel error -y \
  -i "$INPUT_AUDIO" \
  -vn -sn -dn \
  -map_metadata -1 \
  -af "apad=pad_dur=0.2" \
  -ar 8000 \
  -ac 1 \
  -codec:a libmp3lame \
  -b:a 64k \
  -write_xing 0 \
  -id3v2_version 0 \
  -write_id3v1 0 \
  "$OUTPUT_MP3"

CODEC="$(ffprobe -v error -select_streams a:0 -show_entries stream=codec_name -of default=nw=1:nk=1 "$OUTPUT_MP3")"
RATE="$(ffprobe -v error -select_streams a:0 -show_entries stream=sample_rate -of default=nw=1:nk=1 "$OUTPUT_MP3")"
CHANNELS="$(ffprobe -v error -select_streams a:0 -show_entries stream=channels -of default=nw=1:nk=1 "$OUTPUT_MP3")"
BITRATE="$(ffprobe -v error -select_streams a:0 -show_entries stream=bit_rate -of default=nw=1:nk=1 "$OUTPUT_MP3")"

if [ "$CODEC" != "mp3" ] || [ "$RATE" != "8000" ] || [ "$CHANNELS" != "1" ] || [ "$BITRATE" != "64000" ]; then
  echo "unexpected output format: codec=$CODEC sample_rate=$RATE channels=$CHANNELS bitrate=$BITRATE" >&2
  exit 1
fi

printf 'wrote %s: mp3, %s Hz, %s channel, %s bps\n' "$OUTPUT_MP3" "$RATE" "$CHANNELS" "$BITRATE"
