#!/bin/sh
# One local, authenticated temperature read.
set -eu
cd /tmp
work=${KETTLE_RUN_DIR:-${0%/*}}
bin=${0%/*}
libexec=${KETTLE_LIBEXEC_DIR:-$bin}

listener() {
    awk '$4 == "00010000" && $8 ~ /^\/tmp\/(\.\/)?bt-daemon-socket$/ {print $7}' /proc/net/unix
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
    read -r uptime _ < /proc/uptime
    deadline=$((${uptime%%.*} + 35))
    while :; do
        read -r uptime _ < /proc/uptime
        [ "${uptime%%.*}" -lt "$deadline" ] || return 1
        [ "$(pidof bsa_server || true)" = "$server" ] || return 1
        [ "$(listener)" = "$socket_inode" ] || return 1
        if "$0" status >/dev/null; then
            if [ -n "${KETTLE_SAVED_KEY:-}" ]; then
                saved_tmp=$(mktemp "${KETTLE_SAVED_KEY}.XXXXXX")
                if ! cp "$candidate" "$saved_tmp" || ! ln "$saved_tmp" "$KETTLE_SAVED_KEY"; then
                    rm -f "$saved_tmp"
                    return 1
                fi
                rm -f "$saved_tmp"
            else
                : > "$work/pair-verified"
            fi
            # The launcher owns cleanup after this point so the next voice
            # command can use the verified key in the same session.
            trap - EXIT
            return 0
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
