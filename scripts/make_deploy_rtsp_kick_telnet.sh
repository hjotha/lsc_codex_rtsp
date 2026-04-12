#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_BIN="$REPO_ROOT/out/rtsp_kick_arm"
OUT_SCRIPT="$REPO_ROOT/out/deploy_rtsp_kick_to_tmp.telnet"

if [ ! -f "$SRC_BIN" ]; then
  echo "missing $SRC_BIN; run scripts/build_rtsp_kick_anyka.sh first" >&2
  exit 1
fi

python3 - "$SRC_BIN" "$OUT_SCRIPT" <<'PY'
import base64
import pathlib
import sys

src = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2])
blob = base64.b64encode(src.read_bytes()).decode("ascii")

lines = [
    "rm -f /tmp/rtsp_kick /tmp/rtsp_kick.b64",
    "cat >/tmp/rtsp_kick.b64 <<'__RTSP_KICK_B64__'",
]
lines.extend(blob[i:i + 76] for i in range(0, len(blob), 76))
lines.extend([
    "__RTSP_KICK_B64__",
    "base64 -d /tmp/rtsp_kick.b64 >/tmp/rtsp_kick",
    "chmod 755 /tmp/rtsp_kick",
    "ls -l /tmp/rtsp_kick",
    "rm -f /tmp/rtsp_kick.b64",
])

out.write_text("\n".join(lines) + "\n", encoding="ascii")
print(out)
PY
