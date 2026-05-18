#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
usage:
  extract_candidates.sh INPUT.raw OUT_PREFIX [rate] [silence_db] [min_s] [max_s] [pad_s]

Example:
  ./extract_candidates.sh captures/positive.raw captures/jarvis_candidate 8000 -35 0.30 1.20 0.12

Creates OUT_PREFIX_01.raw, OUT_PREFIX_01.wav, ...
USAGE
}

if [ "$#" -lt 2 ]; then
  usage
  exit 2
fi

input="$1"
prefix="$2"
rate="${3:-8000}"
silence_db="${4:--35}"
min_s="${5:-0.30}"
max_s="${6:-1.20}"
pad_s="${7:-0.12}"

if [ ! -f "$input" ]; then
  echo "missing input: $input" >&2
  exit 2
fi

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "ffmpeg is required" >&2
  exit 2
fi

mkdir -p "$(dirname "$prefix")"
tmp_log="$(mktemp)"
tmp_intervals="$(mktemp)"
trap 'rm -f "$tmp_log" "$tmp_intervals"' EXIT

ffmpeg -hide_banner \
  -f s16le \
  -ar "$rate" \
  -ac 1 \
  -i "$input" \
  -af "silencedetect=n=${silence_db}dB:d=0.15" \
  -f null - 2>"$tmp_log" >/dev/null || true

awk -v min_s="$min_s" -v max_s="$max_s" -v pad="$pad_s" '
  function emit(start, stop) {
    dur = stop - start
    if (dur >= min_s && dur <= max_s) {
      padded_start = start - pad
      if (padded_start < 0) {
        padded_start = 0
      }
      padded_dur = dur + (2 * pad)
      start_s = sprintf("%.3f", padded_start)
      dur_s = sprintf("%.3f", padded_dur)
      if (start_s ~ /^\./) {
        start_s = "0" start_s
      }
      if (dur_s ~ /^\./) {
        dur_s = "0" dur_s
      }
      printf "%s %s\n", start_s, dur_s
    }
  }
  /silence_end:/ {
    for (i = 1; i <= NF; i++) {
      if ($i == "silence_end:") {
        prev_end = $(i + 1) + 0
      }
    }
  }
  /silence_start:/ {
    for (i = 1; i <= NF; i++) {
      if ($i == "silence_start:") {
        start = $(i + 1) + 0
        emit(prev_end, start)
      }
    }
  }
' "$tmp_log" > "$tmp_intervals"

count=0
while read -r start dur; do
  [ -n "$start" ] || continue
  count=$((count + 1))
  raw=$(printf '%s_%02d.raw' "$prefix" "$count")
  wav=$(printf '%s_%02d.wav' "$prefix" "$count")
  ffmpeg -hide_banner -loglevel error -y \
    -f s16le \
    -ar "$rate" \
    -ac 1 \
    -ss "$start" \
    -t "$dur" \
    -i "$input" \
    -f s16le \
    "$raw" < /dev/null
  ffmpeg -hide_banner -loglevel error -y \
    -f s16le \
    -ar "$rate" \
    -ac 1 \
    -i "$raw" \
    "$wav" < /dev/null
  bytes=$(wc -c < "$raw")
  printf 'candidate=%02d start=%s dur=%s raw=%s bytes=%s wav=%s\n' \
    "$count" "$start" "$dur" "$raw" "$bytes" "$wav"
done < "$tmp_intervals"

printf 'candidates=%d\n' "$count"
