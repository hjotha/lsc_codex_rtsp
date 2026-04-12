#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "$#" -lt 1 ]; then
  echo "usage: $0 <camera_ip> [telnet_port]" >&2
  exit 2
fi

CAMERA_IP="$1"
TELNET_PORT="${2:-24}"
TMP_TELNET="$REPO_ROOT/out/install_vendor_bootstrap.telnet"
NORMALIZED_BOOT="$REPO_ROOT/out/vendor_rtsp_boot.sh"
NORMALIZED_MD5="$REPO_ROOT/out/vendor_rtsp_boot.md5"

mkdir -p "$REPO_ROOT/out"

bash "$REPO_ROOT/scripts/build_rtsp_kick_anyka.sh" >/dev/null

sed 's/\r$//' "$REPO_ROOT/sdcard/vendor_rtsp_boot.sh" > "$NORMALIZED_BOOT"
sed 's/\r$//' "$REPO_ROOT/sdcard/vendor_rtsp_boot.md5" > "$NORMALIZED_MD5"

python3 "$REPO_ROOT/tools/telnet_upload_file.py" "$CAMERA_IP" \
  "$REPO_ROOT/out/rtsp_kick_arm" \
  /tmp/sd/rtsp_kick \
  --port "$TELNET_PORT" \
  --wait 0.8 \
  --chunk-lines 4 \
  --commands-per-session 1

python3 "$REPO_ROOT/tools/telnet_upload_file.py" "$CAMERA_IP" \
  "$NORMALIZED_BOOT" \
  /tmp/sd/vendor_rtsp_boot.sh \
  --port "$TELNET_PORT" \
  --wait 0.8 \
  --chunk-lines 1 \
  --commands-per-session 1 \
  --mode 755

python3 "$REPO_ROOT/tools/telnet_upload_file.py" "$CAMERA_IP" \
  "$NORMALIZED_MD5" \
  /tmp/sd/vendor_rtsp_boot.md5 \
  --port "$TELNET_PORT" \
  --wait 0.8 \
  --chunk-lines 1 \
  --commands-per-session 1

cat >"$TMP_TELNET" <<'EOF'
if [ ! -e /tmp/sd/custom.sh.pre_vendor_rtsp ]; then
 cp /tmp/sd/custom.sh /tmp/sd/custom.sh.pre_vendor_rtsp
fi
if ! grep -q "vendor_rtsp_boot.sh" /tmp/sd/custom.sh; then
 cat >> /tmp/sd/custom.sh <<'__VENDOR_RTSP_BOOT__'

if [ -x /tmp/sd/vendor_rtsp_boot.sh ]; then
 /tmp/sd/vendor_rtsp_boot.sh
fi
__VENDOR_RTSP_BOOT__
 chmod 755 /tmp/sd/custom.sh
fi
sed -n '1,240p' /tmp/sd/custom.sh
ls -l /tmp/sd/rtsp_kick /tmp/sd/vendor_rtsp_boot.sh /tmp/sd/vendor_rtsp_boot.md5 /tmp/sd/custom.sh
EOF

python3 "$REPO_ROOT/tools/telnet_exec.py" "$CAMERA_IP" \
  --port "$TELNET_PORT" \
  --wait 2 \
  --file "$TMP_TELNET"
