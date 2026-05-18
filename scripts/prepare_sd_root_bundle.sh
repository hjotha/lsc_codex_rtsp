#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
V105_BUNDLE_ROOT="$REPO_ROOT/packages/sd_root_v3.2863.105/root"
V93_BUNDLE_ROOT="$REPO_ROOT/packages/sd_root_v3.2863.93/root"
V105_MD5="c31358a8f598c56073720e96c004fa9c  V3.2863.105 /usr/bin/anyka_ipc"
V93_MD5="87f1683cee35353fb2c2be20353bf59c  V3.2863.93 /usr/bin/anyka_ipc"

prepare_bundle() {
  local bundle_root="$1"
  local md5_line="$2"

  rm -rf "$bundle_root"
  mkdir -p "$bundle_root"

  cp -a "$REPO_ROOT/sdcard/." "$bundle_root/"
  cp "$REPO_ROOT/out/rtsp_kick_arm" "$bundle_root/rtsp_kick"
  printf '%s\n' "$md5_line" > "$bundle_root/vendor_rtsp_boot.md5"

  chmod 755 \
    "$bundle_root/hostapd" \
    "$bundle_root/rtsp_kick" \
    "$bundle_root/hack.sh" \
    "$bundle_root/custom.sh" \
    "$bundle_root/vendor_rtsp_boot.sh"
}

bash "$REPO_ROOT/scripts/build_rtsp_kick_anyka.sh" >/dev/null

cp "$REPO_ROOT/out/rtsp_kick_arm" "$REPO_ROOT/sdcard/rtsp_kick"

prepare_bundle "$V105_BUNDLE_ROOT" "$V105_MD5"
prepare_bundle "$V93_BUNDLE_ROOT" "$V93_MD5"

printf 'Prepared SD root bundles in:\n  %s\n  %s\n' "$V105_BUNDLE_ROOT" "$V93_BUNDLE_ROOT"
