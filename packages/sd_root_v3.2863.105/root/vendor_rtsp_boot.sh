#!/bin/sh

LOG_PATH="/tmp/vendor_rtsp_boot.log"
STATE_PATH="/tmp/vendor_rtsp_boot.done"
UNSUPPORTED_PATH="/tmp/vendor_rtsp_boot.unsupported"
TMP_KICK="/tmp/rtsp_kick"
SD_KICK="/tmp/sd/rtsp_kick"
EXPECTED_MD5_FILE="/tmp/sd/vendor_rtsp_boot.md5"
DEFAULT_EXPECTED_MD5="c31358a8f598c56073720e96c004fa9c"
ALLOW_UNSUPPORTED_MARKER="/tmp/sd/vendor_rtsp_boot.allow_unsupported"

log_line() {
    echo "[$(date +%Y-%m-%d\ %H:%M:%S)] $*" >> "$LOG_PATH"
}

read_expected_md5() {
    local value=""

    if [ -r "$EXPECTED_MD5_FILE" ]; then
        value="$(sed -n '1{s/[[:space:]].*$//;p;q;}' "$EXPECTED_MD5_FILE")"
    fi

    if [ -n "$value" ]; then
        echo "$value"
        return 0
    fi

    echo "$DEFAULT_EXPECTED_MD5"
    return 0
}

compute_file_md5() {
    local target="$1"

    md5sum "$target" 2>/dev/null | awk 'NR == 1 { print $1 }'
}

check_supported_binary() {
    local pid="$1"
    local exe_path=""
    local actual_md5=""
    local expected_md5=""

    if [ -e "$ALLOW_UNSUPPORTED_MARKER" ]; then
        log_line "allow_unsupported marker present; skipping md5 guard"
        return 0
    fi

    exe_path="$(readlink "/proc/$pid/exe" 2>/dev/null)"
    if [ -z "$exe_path" ]; then
        log_line "could not resolve /proc/$pid/exe"
        return 1
    fi

    actual_md5="$(compute_file_md5 "$exe_path")"
    if [ -z "$actual_md5" ]; then
        log_line "could not compute md5 for $exe_path"
        return 1
    fi

    expected_md5="$(read_expected_md5)"
    if [ "$actual_md5" != "$expected_md5" ]; then
        log_line "unsupported anyka_ipc md5=$actual_md5 expected=$expected_md5 path=$exe_path; refusing to patch"
        touch "$UNSUPPORTED_PATH"
        return 1
    fi

    log_line "confirmed supported anyka_ipc md5=$actual_md5 path=$exe_path"
    return 0
}

ports_ready() {
    netstat -ltn 2>/dev/null | grep -q ':88 ' &&
    netstat -ltn 2>/dev/null | grep -q ':89 '
}

ensure_rtsp_kick() {
    if [ -x "$TMP_KICK" ]; then
        return 0
    fi
    if [ ! -x "$SD_KICK" ]; then
        log_line "missing $SD_KICK"
        return 1
    fi
    cp "$SD_KICK" "$TMP_KICK" || return 1
    chmod 755 "$TMP_KICK" || return 1
    log_line "copied rtsp_kick from SD to /tmp"
    return 0
}

main() {
    PID=""

    if [ -e "$STATE_PATH" ] || [ -e "$UNSUPPORTED_PATH" ]; then
        exit 0
    fi

    ensure_rtsp_kick || exit 0

    PID="$(pidof anyka_ipc 2>/dev/null | awk '{print $1}')"
    if [ -z "$PID" ]; then
        log_line "anyka_ipc not ready yet"
        exit 0
    fi

    check_supported_binary "$PID" || exit 0

    if ! ports_ready; then
        log_line "starting stock RTSP worker for pid $PID"
        "$TMP_KICK" "$PID" --verbose >> "$LOG_PATH" 2>&1 || true
        sleep 1
    fi

    log_line "installing video callback chain for pid $PID"
    if "$TMP_KICK" "$PID" --verbose --install-video-chain --no-start-call >> "$LOG_PATH" 2>&1; then
        if ports_ready; then
            touch "$STATE_PATH"
            log_line "vendor RTSP bootstrap finished successfully"
        fi
        exit 0
    fi

    if ports_ready; then
        touch "$STATE_PATH"
        log_line "ports 88 and 89 are listening and the chain install returned non-zero; assuming this boot is already patched"
    else
        log_line "vendor RTSP ports are not ready yet; will retry next custom.sh cycle"
    fi
}

main "$@"
