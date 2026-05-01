#!/bin/sh

LOG_PATH="/tmp/vendor_rtsp_boot.log"
STATE_PATH="/tmp/vendor_rtsp_boot.done"
UNSUPPORTED_PATH="/tmp/vendor_rtsp_boot.unsupported"
LAST_ATTEMPT_PATH="/tmp/vendor_rtsp_boot.last_attempt"
TMP_KICK="/tmp/rtsp_kick"
SD_KICK="/tmp/sd/rtsp_kick"
EXPECTED_MD5_FILE="/tmp/sd/vendor_rtsp_boot.md5"
DEFAULT_EXPECTED_MD5="c31358a8f598c56073720e96c004fa9c"
ALLOW_UNSUPPORTED_MARKER="/tmp/sd/vendor_rtsp_boot.allow_unsupported"
MIN_RETRY_SECONDS=30

# Known-good firmware MD5s and their offsets. The md5 file on SD may override the
# default list, but the builtin table covers the firmware versions we have
# validated directly against hardware.
MD5_V105="c31358a8f598c56073720e96c004fa9c"
MD5_V93="87f1683cee35353fb2c2be20353bf59c"

log_line() {
    echo "[$(date +%Y-%m-%d\ %H:%M:%S)] $*" >> "$LOG_PATH"
}

# Emit the rtsp_kick offset arguments for a given anyka_ipc md5. Firmware
# builds we do not recognize fall back to the V3.2863.105 defaults baked
# into the rtsp_kick binary, which means no extra args.
offsets_for_md5() {
    case "$1" in
        "$MD5_V93")
            echo "--func-vaddr 0x00091548 --guard-vaddr 0x0051ab34 --malloc-vaddr 0x000607b4 --video-send-vaddr 0x00091064 --video-slot0-vaddr 0x0051abc0 --video-slot1-vaddr 0x0051abfc --expected-video-cb0 0x000a7124 --expected-video-cb1 0x000a723c"
            ;;
        "$MD5_V105"|*)
            echo ""
            ;;
    esac
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

    # Write md5 to a shared variable for downstream steps
    ANYKA_MD5="$actual_md5"

    if [ -e "$ALLOW_UNSUPPORTED_MARKER" ]; then
        log_line "allow_unsupported marker present; continuing with md5=$actual_md5"
        return 0
    fi

    case "$actual_md5" in
        "$MD5_V93"|"$MD5_V105")
            log_line "confirmed supported anyka_ipc md5=$actual_md5 path=$exe_path"
            return 0
            ;;
    esac

    # Check if user provided override via md5 file
    local expected_md5
    expected_md5="$(read_expected_md5)"
    if [ "$actual_md5" = "$expected_md5" ]; then
        log_line "md5 $actual_md5 matches vendor_rtsp_boot.md5 override"
        return 0
    fi

    log_line "unsupported anyka_ipc md5=$actual_md5 path=$exe_path; refusing to patch"
    touch "$UNSUPPORTED_PATH"
    return 1
}

ports_ready() {
    local out
    out="$(netstat -ltn 2>/dev/null)"
    echo "$out" | grep -q ':88 ' && echo "$out" | grep -q ':89 '
}

read_state_pid() {
    if [ -r "$STATE_PATH" ]; then
        sed -n '1{s/[^0-9].*$//;p;q;}' "$STATE_PATH"
    fi
}

mark_state() {
    local pid="$1"

    echo "$pid" > "$STATE_PATH"
}

current_epoch() {
    local now=""

    now="$(date +%s 2>/dev/null)"
    case "$now" in
        ''|*[!0-9]*)
            echo 0
            ;;
        *)
            echo "$now"
            ;;
    esac
}

retry_allowed() {
    local now=""
    local last=""

    now="$(current_epoch)"
    if [ "$now" -le 0 ]; then
        return 0
    fi
    if [ ! -r "$LAST_ATTEMPT_PATH" ]; then
        return 0
    fi

    last="$(sed -n '1{s/[^0-9].*$//;p;q;}' "$LAST_ATTEMPT_PATH")"
    case "$last" in
        ''|*[!0-9]*)
            return 0
            ;;
    esac

    if [ $((now - last)) -lt "$MIN_RETRY_SECONDS" ]; then
        return 1
    fi

    return 0
}

note_attempt() {
    local now=""

    now="$(current_epoch)"
    if [ "$now" -gt 0 ]; then
        echo "$now" > "$LAST_ATTEMPT_PATH"
    fi
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
    PREV_PID=""
    RECOVERY_MODE=0
    ANYKA_MD5=""
    OFFSETS=""

    if [ -e "$UNSUPPORTED_PATH" ]; then
        exit 0
    fi

    ensure_rtsp_kick || exit 0

    PID="$(pidof anyka_ipc 2>/dev/null | awk '{print $1}')"
    if [ -z "$PID" ]; then
        log_line "anyka_ipc not ready yet"
        exit 0
    fi

    PREV_PID="$(read_state_pid)"
    if [ -e "$STATE_PATH" ]; then
        if [ -n "$PREV_PID" ] && [ "$PREV_PID" != "$PID" ]; then
            RECOVERY_MODE=1
            log_line "anyka_ipc pid changed from $PREV_PID to $PID; re-arming vendor RTSP bootstrap"
        elif ports_ready; then
            exit 0
        else
            RECOVERY_MODE=1
            log_line "ports 88 and 89 disappeared after earlier success; attempting RTSP recovery for pid $PID"
        fi
    fi

    check_supported_binary "$PID" || exit 0

    OFFSETS="$(offsets_for_md5 "$ANYKA_MD5")"

    if ! retry_allowed; then
        exit 0
    fi
    note_attempt

    if ! ports_ready; then
        log_line "starting stock RTSP worker for pid $PID (md5=$ANYKA_MD5)"
        if [ "$RECOVERY_MODE" -eq 1 ]; then
            $TMP_KICK "$PID" --verbose --no-guard-check $OFFSETS >> "$LOG_PATH" 2>&1 || true
        else
            $TMP_KICK "$PID" --verbose $OFFSETS >> "$LOG_PATH" 2>&1 || true
        fi
        sleep 1
    fi

    log_line "installing video callback chain for pid $PID"
    if $TMP_KICK "$PID" --verbose --install-video-chain --no-start-call $OFFSETS >> "$LOG_PATH" 2>&1; then
        if ports_ready; then
            mark_state "$PID"
            log_line "vendor RTSP bootstrap finished successfully"
        fi
        exit 0
    fi

    if ports_ready; then
        mark_state "$PID"
        log_line "ports 88 and 89 are listening and the chain install returned non-zero; assuming this boot is already patched"
    else
        log_line "vendor RTSP ports are not ready yet; will retry next custom.sh cycle"
    fi
}

main "$@"
