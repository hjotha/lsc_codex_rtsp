#!/bin/bash
# Unit tests for vendor_rtsp_boot.sh
#
# Run: bash tests/test_vendor_rtsp_boot.sh

set -euo pipefail

TESTS_RUN=0
TESTS_PASSED=0
TMPDIR_BASE=""
SOURCEABLE=""
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENDOR_SCRIPT="$SCRIPT_DIR/sdcard/vendor_rtsp_boot.sh"
DEFAULT_EXPECTED_MD5_VALUE="c31358a8f598c56073720e96c004fa9c"

setup() {
    TMPDIR_BASE="$(mktemp -d)"
    mkdir -p "$TMPDIR_BASE/bin"
    export PATH="$TMPDIR_BASE/bin:$PATH"
}

teardown() {
    rm -rf "$TMPDIR_BASE"
}

trap teardown EXIT

pass() {
    TESTS_PASSED=$((TESTS_PASSED + 1))
    printf "  %-60s OK\n" "$1"
}

fail() {
    printf "  %-60s FAIL: %s\n" "$1" "$2"
}

run_test() {
    TESTS_RUN=$((TESTS_RUN + 1))
}

write_mock() {
    local name="$1"
    shift

    printf '%s\n' "$@" > "$TMPDIR_BASE/bin/$name"
    chmod +x "$TMPDIR_BASE/bin/$name"
}

reset_runtime() {
    rm -rf "$TMPDIR_BASE/runtime"
    mkdir -p "$TMPDIR_BASE/runtime/sd"
    rm -f "$TMPDIR_BASE/bin/"*

    LOG_PATH="$TMPDIR_BASE/runtime/vendor_rtsp_boot.log"
    STATE_PATH="$TMPDIR_BASE/runtime/vendor_rtsp_boot.done"
    UNSUPPORTED_PATH="$TMPDIR_BASE/runtime/vendor_rtsp_boot.unsupported"
    LAST_ATTEMPT_PATH="$TMPDIR_BASE/runtime/vendor_rtsp_boot.last_attempt"
    TMP_KICK="$TMPDIR_BASE/runtime/rtsp_kick"
    SD_KICK="$TMPDIR_BASE/runtime/sd/rtsp_kick"
    EXPECTED_MD5_FILE="$TMPDIR_BASE/runtime/sd/vendor_rtsp_boot.md5"
    ALLOW_UNSUPPORTED_MARKER="$TMPDIR_BASE/runtime/sd/vendor_rtsp_boot.allow_unsupported"
    DEFAULT_EXPECTED_MD5="$DEFAULT_EXPECTED_MD5_VALUE"
    MIN_RETRY_SECONDS=30

    export LOG_PATH
    export STATE_PATH
    export UNSUPPORTED_PATH
    export LAST_ATTEMPT_PATH
    export TMP_KICK
    export SD_KICK
    export EXPECTED_MD5_FILE
    export ALLOW_UNSUPPORTED_MARKER
    export DEFAULT_EXPECTED_MD5
    export MIN_RETRY_SECONDS
}

prepare_sourceable_script() {
    if [ ! -f "$VENDOR_SCRIPT" ]; then
        echo "ERROR: $VENDOR_SCRIPT not found"
        exit 2
    fi

    SOURCEABLE="$TMPDIR_BASE/vendor_rtsp_boot_test.sh"
    sed 's/^main "\$@"$/# main "$@" (disabled for testing)/' "$VENDOR_SCRIPT" > "$SOURCEABLE"
    # shellcheck disable=SC1090
    source "$SOURCEABLE"
}

write_common_runtime_mocks() {
    write_mock pidof \
        '#!/bin/sh' \
        'if [ "$1" = "anyka_ipc" ]; then' \
        '    echo "${MOCK_PIDOF_OUTPUT:-567}"' \
        'fi'

    write_mock readlink \
        '#!/bin/sh' \
        'echo "${MOCK_EXE_PATH:-/usr/bin/anyka_ipc}"'

    write_mock md5sum \
        '#!/bin/sh' \
        'if [ -n "${MD5_CALLED_FILE:-}" ]; then' \
        '    echo called >> "$MD5_CALLED_FILE"' \
        'fi' \
        'echo "${MOCK_MD5:-c31358a8f598c56073720e96c004fa9c}  $1"'

    write_mock sleep \
        '#!/bin/sh' \
        'exit 0'
}

write_netstat_mock() {
    write_mock netstat \
        '#!/bin/sh' \
        'if [ -f "${PORTS_UP_FILE:-}" ]; then' \
        '    echo "tcp   0   0 0.0.0.0:88    0.0.0.0:*   LISTEN"' \
        '    echo "tcp   0   0 0.0.0.0:89    0.0.0.0:*   LISTEN"' \
        'fi' \
        'echo "tcp   0   0 0.0.0.0:6668  0.0.0.0:*   LISTEN"'
}

write_rtsp_kick_stub() {
    printf '%s\n' \
        '#!/bin/sh' \
        'if [ -n "${RTSP_KICK_ARGS_FILE:-}" ]; then' \
        '    echo "$*" >> "$RTSP_KICK_ARGS_FILE"' \
        'fi' \
        'case " $* " in' \
        '  *" --install-video-chain "*)' \
        '    if [ -n "${PORTS_UP_FILE:-}" ]; then' \
        '        : > "$PORTS_UP_FILE"' \
        '    fi' \
        '    exit 0' \
        '    ;;' \
        '  *)' \
        '    exit 0' \
        '    ;;' \
        'esac' \
        > "$TMP_KICK"
    chmod 755 "$TMP_KICK"
}

run_main_subshell() {
    ( main )
}

setup
prepare_sourceable_script
reset_runtime

echo ""
echo "=== vendor_rtsp_boot.sh unit tests ==="
echo ""

run_test
name="read_expected_md5: returns default when no file exists"
rm -f "$EXPECTED_MD5_FILE"
result="$(read_expected_md5)"
if [ "$result" = "$DEFAULT_EXPECTED_MD5_VALUE" ]; then
    pass "$name"
else
    fail "$name" "got '$result'"
fi

run_test
name="read_expected_md5: reads first word from file"
echo "aabbccdd  /some/path" > "$EXPECTED_MD5_FILE"
result="$(read_expected_md5)"
if [ "$result" = "aabbccdd" ]; then
    pass "$name"
else
    fail "$name" "got '$result'"
fi

run_test
name="read_expected_md5: strips trailing content after space"
echo "deadbeef01234567deadbeef01234567 filename.bin" > "$EXPECTED_MD5_FILE"
result="$(read_expected_md5)"
if [ "$result" = "deadbeef01234567deadbeef01234567" ]; then
    pass "$name"
else
    fail "$name" "got '$result'"
fi

run_test
name="read_expected_md5: falls back to default on empty file"
echo "" > "$EXPECTED_MD5_FILE"
result="$(read_expected_md5)"
if [ "$result" = "$DEFAULT_EXPECTED_MD5_VALUE" ]; then
    pass "$name"
else
    fail "$name" "got '$result'"
fi

run_test
name="read_expected_md5: handles whitespace-only file"
echo "   " > "$EXPECTED_MD5_FILE"
result="$(read_expected_md5)"
if [ "$result" = "$DEFAULT_EXPECTED_MD5_VALUE" ]; then
    pass "$name"
else
    fail "$name" "got '$result'"
fi

run_test
name="compute_file_md5: computes correct hash"
echo -n "test content" > "$TMPDIR_BASE/testfile"
expected="$(md5sum "$TMPDIR_BASE/testfile" | awk '{print $1}')"
result="$(compute_file_md5 "$TMPDIR_BASE/testfile")"
if [ "$result" = "$expected" ]; then
    pass "$name"
else
    fail "$name" "got '$result' expected '$expected'"
fi

run_test
name="compute_file_md5: returns empty on missing file"
result="$(compute_file_md5 "$TMPDIR_BASE/nonexistent" || true)"
if [ -z "$result" ]; then
    pass "$name"
else
    fail "$name" "got '$result'"
fi

run_test
name="ports_ready: true when both 88 and 89 are listening"
write_mock netstat \
    '#!/bin/sh' \
    'echo "tcp   0   0 0.0.0.0:88    0.0.0.0:*   LISTEN"' \
    'echo "tcp   0   0 0.0.0.0:89    0.0.0.0:*   LISTEN"' \
    'echo "tcp   0   0 0.0.0.0:6668  0.0.0.0:*   LISTEN"'
if ports_ready; then
    pass "$name"
else
    fail "$name" "returned false"
fi

run_test
name="ports_ready: false when only 88 is listening"
write_mock netstat \
    '#!/bin/sh' \
    'echo "tcp   0   0 0.0.0.0:88    0.0.0.0:*   LISTEN"' \
    'echo "tcp   0   0 0.0.0.0:*    0.0.0.0:*   LISTEN"' \
    'echo "tcp   0   0 0.0.0.0:6668  0.0.0.0:*   LISTEN"'
if ports_ready; then
    fail "$name" "returned true"
else
    pass "$name"
fi

run_test
name="ports_ready: false when neither port is listening"
write_mock netstat \
    '#!/bin/sh' \
    'echo "tcp   0   0 0.0.0.0:6668  0.0.0.0:*   LISTEN"'
if ports_ready; then
    fail "$name" "returned true"
else
    pass "$name"
fi

run_test
name="ports_ready: does not false-match port 880"
write_mock netstat \
    '#!/bin/sh' \
    'echo "tcp   0   0 0.0.0.0:880   0.0.0.0:*   LISTEN"' \
    'echo "tcp   0   0 0.0.0.0:890   0.0.0.0:*   LISTEN"'
if ports_ready; then
    fail "$name" "matched 880/890 as 88/89"
else
    pass "$name"
fi

run_test
name="read_state_pid: reads numeric pid from state file"
echo "567 restored" > "$STATE_PATH"
result="$(read_state_pid)"
if [ "$result" = "567" ]; then
    pass "$name"
else
    fail "$name" "got '$result'"
fi

run_test
name="mark_state: writes pid to state file"
mark_state 4321
result="$(cat "$STATE_PATH")"
if [ "$result" = "4321" ]; then
    pass "$name"
else
    fail "$name" "got '$result'"
fi

run_test
name="retry_allowed: allows first attempt when file is missing"
rm -f "$LAST_ATTEMPT_PATH"
if retry_allowed; then
    pass "$name"
else
    fail "$name" "returned false"
fi

run_test
name="note_attempt: stores current epoch"
write_mock date \
    '#!/bin/sh' \
    'if [ "$1" = "+%s" ]; then' \
    '    echo 1700000000' \
    'else' \
    '    echo "2026-04-12 21:00:00"' \
    'fi'
note_attempt
result="$(cat "$LAST_ATTEMPT_PATH")"
if [ "$result" = "1700000000" ]; then
    pass "$name"
else
    fail "$name" "got '$result'"
fi

run_test
name="retry_allowed: blocks attempts inside backoff window"
echo "1700000000" > "$LAST_ATTEMPT_PATH"
if retry_allowed; then
    fail "$name" "returned true"
else
    pass "$name"
fi

run_test
name="retry_allowed: allows attempts after backoff window"
echo "1699999900" > "$LAST_ATTEMPT_PATH"
if retry_allowed; then
    pass "$name"
else
    fail "$name" "returned false"
fi

reset_runtime

run_test
name="ensure_rtsp_kick: succeeds when TMP_KICK already exists"
echo "#!/bin/sh" > "$TMP_KICK"
chmod +x "$TMP_KICK"
if ensure_rtsp_kick; then
    pass "$name"
else
    fail "$name" "returned failure"
fi
rm -f "$TMP_KICK"

run_test
name="ensure_rtsp_kick: copies from SD when TMP missing"
echo "#!/bin/sh" > "$SD_KICK"
chmod +x "$SD_KICK"
if ensure_rtsp_kick; then
    if [ -x "$TMP_KICK" ]; then
        pass "$name"
    else
        fail "$name" "did not create TMP_KICK"
    fi
else
    fail "$name" "returned failure"
fi

run_test
name="ensure_rtsp_kick: fails when SD_KICK missing too"
rm -f "$TMP_KICK" "$SD_KICK"
if ensure_rtsp_kick; then
    fail "$name" "returned success"
else
    pass "$name"
fi

run_test
name="UNSUPPORTED_PATH prevents re-run"
reset_runtime
write_common_runtime_mocks
write_netstat_mock
write_rtsp_kick_stub
touch "$UNSUPPORTED_PATH"
RTSP_KICK_ARGS_FILE="$TMPDIR_BASE/runtime/kick_args.log"
export RTSP_KICK_ARGS_FILE
run_main_subshell
if [ ! -e "$RTSP_KICK_ARGS_FILE" ]; then
    pass "$name"
else
    fail "$name" "unexpected rtsp_kick execution"
fi

run_test
name="healthy state skips md5 check and re-patch"
reset_runtime
write_common_runtime_mocks
write_netstat_mock
write_rtsp_kick_stub
mark_state 567
PORTS_UP_FILE="$TMPDIR_BASE/runtime/ports_up"
MD5_CALLED_FILE="$TMPDIR_BASE/runtime/md5_called.log"
RTSP_KICK_ARGS_FILE="$TMPDIR_BASE/runtime/kick_args.log"
export PORTS_UP_FILE
export MD5_CALLED_FILE
export RTSP_KICK_ARGS_FILE
: > "$PORTS_UP_FILE"
run_main_subshell
if [ -e "$MD5_CALLED_FILE" ]; then
    fail "$name" "md5 guard ran in healthy steady state"
elif [ -e "$RTSP_KICK_ARGS_FILE" ]; then
    fail "$name" "rtsp_kick ran in healthy steady state"
else
    pass "$name"
fi

run_test
name="recovery mode re-arms RTSP when pid changes"
reset_runtime
write_common_runtime_mocks
write_netstat_mock
write_rtsp_kick_stub
mark_state 111
PORTS_UP_FILE="$TMPDIR_BASE/runtime/ports_up"
RTSP_KICK_ARGS_FILE="$TMPDIR_BASE/runtime/kick_args.log"
MD5_CALLED_FILE="$TMPDIR_BASE/runtime/md5_called.log"
export PORTS_UP_FILE
export RTSP_KICK_ARGS_FILE
export MD5_CALLED_FILE
run_main_subshell
if ! grep -q '^567 --verbose --no-guard-check$' "$RTSP_KICK_ARGS_FILE"; then
    fail "$name" "missing recovery start call"
elif ! grep -q '^567 --verbose --install-video-chain --no-start-call$' "$RTSP_KICK_ARGS_FILE"; then
    fail "$name" "missing video chain install call"
elif [ "$(cat "$STATE_PATH")" != "567" ]; then
    fail "$name" "state file not updated to current pid"
else
    pass "$name"
fi

run_test
name="recent retry timestamp suppresses another kick attempt"
reset_runtime
write_common_runtime_mocks
write_netstat_mock
write_rtsp_kick_stub
write_mock date \
    '#!/bin/sh' \
    'if [ "$1" = "+%s" ]; then' \
    '    echo 1700000000' \
    'else' \
    '    echo "2026-04-12 21:00:00"' \
    'fi'
echo "1699999990" > "$LAST_ATTEMPT_PATH"
RTSP_KICK_ARGS_FILE="$TMPDIR_BASE/runtime/kick_args.log"
export RTSP_KICK_ARGS_FILE
run_main_subshell
if [ ! -e "$RTSP_KICK_ARGS_FILE" ]; then
    pass "$name"
else
    fail "$name" "rtsp_kick ran despite backoff"
fi

reset_runtime

run_test
name="log_line: appends timestamped entry"
log_line "test message"
if grep -q "test message" "$LOG_PATH"; then
    pass "$name"
else
    fail "$name" "message not found in log"
fi

run_test
name="log_line: includes timestamp format"
if grep -qE '^\[20[0-9]{2}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\]' "$LOG_PATH"; then
    pass "$name"
else
    fail "$name" "timestamp format wrong"
fi

echo ""
echo "--- $TESTS_PASSED/$TESTS_RUN tests passed ---"
echo ""

if [ "$TESTS_PASSED" -eq "$TESTS_RUN" ]; then
    exit 0
else
    exit 1
fi
