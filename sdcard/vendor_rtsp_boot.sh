#!/bin/sh

LOG_PATH="/tmp/vendor_rtsp_boot.log"
STATE_PATH="/tmp/vendor_rtsp_boot.done"
TMP_KICK="/tmp/rtsp_kick"
SD_KICK="/tmp/sd/rtsp_kick"

log_line() {
    echo "[$(date +%Y-%m-%d\ %H:%M:%S)] $*" >> "$LOG_PATH"
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

    if [ -e "$STATE_PATH" ]; then
        exit 0
    fi

    ensure_rtsp_kick || exit 0

    PID="$(pidof anyka_ipc 2>/dev/null | awk '{print $1}')"
    if [ -z "$PID" ]; then
        log_line "anyka_ipc not ready yet"
        exit 0
    fi

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
