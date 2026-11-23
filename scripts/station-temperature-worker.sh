#!/bin/sh
# One local, authenticated temperature read.
set -eu
cd /tmp
work=${KETTLE_RUN_DIR:-${0%/*}}
bin=${0%/*}
libexec=${KETTLE_LIBEXEC_DIR:-$bin}

require_commands() {
    for command in "$@"; do
        command -v "$command" >/dev/null 2>&1 || {
            printf 'Missing required command: %s\n' "$command" >&2
            exit 1
        }
    done
}
require_commands awk pidof mktemp grep wc ln cp rm rmdir sleep

# The voice client exports this value when it starts the pair job; 75000 ms is
# the fallback for manual pairing sessions.
pair_timeout_ms() {
    value=${KETTLE_PAIR_TIMEOUT_MS:-75000}
    case "$value" in
        ''|*[!0-9]*|0*)
            printf 'KETTLE_PAIR_TIMEOUT_MS must be a positive integer\n' >&2
            return 1
            ;;
    esac
    if [ "${#value}" -gt 7 ] || [ "$value" -gt 3600000 ]; then
        printf 'KETTLE_PAIR_TIMEOUT_MS is too large\n' >&2
        return 1
    fi
    printf '%s\n' "$value"
}

listener() {
    awk '$4 == "00010000" && $8 ~ /^\/tmp\/(\.\/)?bt-daemon-socket$/ {print $7}' /proc/net/unix
}

pair_log() {
    _pair_event=$1
    _pair_detail=${2:-}
    read -r _pair_uptime _ < /proc/uptime
    if [ -n "$_pair_detail" ]; then
        printf 'pair_worker %s uptime_s=%s %s\n' \
            "$_pair_event" "${_pair_uptime%%.*}" "$_pair_detail" >&2
    else
        printf 'pair_worker %s uptime_s=%s\n' \
            "$_pair_event" "${_pair_uptime%%.*}" >&2
    fi
}

pair_key() {
    candidate=${KETTLE_PAIR_KEY:-}
    test -n "$candidate"
    test -f "$candidate"
    test ! -L "$candidate"
    test "$(wc -c < "$candidate")" -eq 8
    server=$(pidof bsa_server)
    socket_inode=$(listener)
    test -n "$socket_inode"
    key_path=${KETTLE_RUN_DIR:+$KETTLE_RUN_DIR/key}
    key_path=${key_path:-/tmp/ble-temp/key}
    if [ -n "${KETTLE_SAVED_KEY:-}" ]; then
        ln -s "$candidate" "$key_path"
    else
        ln "$candidate" "$key_path"
    fi
    trap 'rm -f "$key_path"' EXIT
    trap 'exit 143' HUP INT TERM
    export KETTLE_KEY_AVAILABLE=1
    timeout_ms=$(pair_timeout_ms) || exit 2
    pair_seconds=$(( (timeout_ms + 999) / 1000 ))
    read -r uptime _ < /proc/uptime
    start_s=${uptime%%.*}
    deadline=$((start_s + pair_seconds))
    attempts=0
    pair_log start "timeout_ms=$timeout_ms server_pid=$server socket_inode=$socket_inode"
    while :; do
        read -r uptime _ < /proc/uptime
        now_s=${uptime%%.*}
        elapsed=$((now_s - start_s))
        if [ "$now_s" -ge "$deadline" ]; then
            pair_log timeout "attempts=$attempts elapsed_s=$elapsed deadline_s=$deadline"
            return 1
        fi
        current_server=$(pidof bsa_server || true)
        if [ "$current_server" != "$server" ]; then
            pair_log server_changed "from=$server to=$current_server attempts=$attempts"
            return 1
        fi
        current_socket=$(listener)
        if [ "$current_socket" != "$socket_inode" ]; then
            pair_log socket_changed "from=$socket_inode to=$current_socket attempts=$attempts"
            return 1
        fi
        attempts=$((attempts + 1))
        if "$0" status >/dev/null; then
            pair_log status_ok "attempt=$attempts elapsed_s=$elapsed"
            if [ -n "${KETTLE_SAVED_KEY:-}" ]; then
                saved_tmp=$(mktemp "${KETTLE_SAVED_KEY}.XXXXXX")
                if ! cp "$candidate" "$saved_tmp" || ! ln "$saved_tmp" "$KETTLE_SAVED_KEY"; then
                    rm -f "$saved_tmp"
                    pair_log key_save_failed "path=$KETTLE_SAVED_KEY"
                    return 1
                fi
                rm -f "$saved_tmp"
                pair_log key_saved "path=$KETTLE_SAVED_KEY"
            else
                : > "$work/pair-verified"
            fi
            pair_log verified "attempts=$attempts elapsed_s=$elapsed"
            # The launcher owns cleanup after this point so the next voice
            # command can use the verified key in the same session.
            trap - EXIT
            return 0
        fi
        if [ "$attempts" -le 2 ] || [ $((attempts % 5)) -eq 0 ]; then
            pair_log status_failed "attempt=$attempts elapsed_s=$elapsed"
        fi
        sleep 1
    done
}

case "${KETTLE_ADDRESS:-}" in
    [[:xdigit:]][[:xdigit:]]:[[:xdigit:]][[:xdigit:]]:[[:xdigit:]][[:xdigit:]]:[[:xdigit:]][[:xdigit:]]:[[:xdigit:]][[:xdigit:]]:[[:xdigit:]][[:xdigit:]]) ;;
    *) printf 'Set the kettle BLE address in device.conf\n' >&2; exit 21 ;;
esac

if [ "${1:-}" = pair ]; then
    pair_key
    exit
fi
operation=${1:-temperature}
if [ "$operation" = heat ]; then
    case "${2:-}" in
        3[5-9]|[4-8][0-9]|90) ;;
        *) exit 2 ;;
    esac
fi
if [ ! -r /tmp/ble-temp/key ] || [ "${KETTLE_KEY_AVAILABLE:-1}" = 0 ]; then
    printf 'Ready for Sky key missing\n' >&2
    exit 20
fi
probes="$libexec/${KETTLE_PROBE_PREFIX:-}"
test -x "${probes}discovery"
case "$operation" in
    temperature|status) probe="${probes}gatt-temperature" ;;
    heat) probe="${probes}gatt-boil" ;;
    stop|boil) probe="${probes}gatt-$operation" ;;
    *) exit 2 ;;
esac
test -x "$probe"

umask 077
run=$(mktemp -d "$work/read.XXXXXX")
trap 'rm -f "$run/devices" "$run/probe.log"; rmdir "$run"' EXIT

mark() {
    read -r uptime _ < /proc/uptime
    printf 'temperature_worker %s uptime_s=%s\n' "$1" "$uptime" >&2
}

run_probe() {
    mark gatt_start
    status=0
    "$probe" "$KETTLE_ADDRESS" "$@" >/dev/null 2>"$run/probe.log" || status=$?
    mark gatt_end
}


server=$(pidof bsa_server)
socket_inode=$(listener)
test -n "$socket_inode"
export LD_PRELOAD=libpthread.so.0 LD_LIBRARY_PATH=/system/workdir/lib

if [ "$operation" = heat ]; then set -- "$2"; else set --; fi
run_probe "$@"
if [ "$status" -ne 0 ] && grep -q '^GATT open failed: ' "$run/probe.log"; then
    grep '^GATT open failed: ' "$run/probe.log" >&2
    test "$(pidof bsa_server || true)" = "$server"
    test "$(listener)" = "$socket_inode"
    mark scan_start
    "${probes}discovery" >"$run/devices" || exit 1
    mark scan_end
    if ! grep -aqF 'RFS-KKL003' "$run/devices"; then
        printf 'Kettle absent from fallback scan\n' >&2
        exit 1
    fi
    test "$(pidof bsa_server || true)" = "$server"
    test "$(listener)" = "$socket_inode"
    run_probe "$@"
fi
if [ "$status" -ne 0 ]; then
    printf 'Kettle probe exit=%s\n' "$status" >&2
    exit 1
fi
case "$operation" in
    status)
        awk '
            /^heating_on: [01]$/ { heating = $2 }
            /^temperature_c: [0-9]+$/ { temperature = $2 }
            /^target_temp_c: [0-9]+$/ { target = $2 }
            END {
                if (heating == "" || temperature == "") exit 1
                if (heating == 1 && target + 0 > 0) print heating, temperature, target
                else print heating, temperature
            }
        ' "$run/probe.log"
        exit
        ;;
    temperature) pattern='^temperature_c: [0-9]+$' ;;
    stop) pattern='^heating_result: (idle|stopped)$' ;;
    boil) pattern='^boil_result: (busy|started)$' ;;
    heat) pattern='^boil_result: (busy|started|reached)$' ;;
esac
awk -v expected="$pattern" '$0 ~ expected { print $2; found = 1 } END { if (!found) exit 1 }' "$run/probe.log"
