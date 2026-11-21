#!/bin/sh
set -eu
umask 077
yam_root=${YAM_ROOT:-/data/yam}
entry=$(readlink -f "$0")
pkg=${YAM_PKG:-${entry%/bin/*}}
runtime=${YAM_RUN:-/tmp/yam}/lynx.redmond-kettle
pidfile=$runtime/pid

managed_pid() {
    [ -r "$pidfile" ] && read -r pid started < "$pidfile" || return 1
    case "$pid" in ''|*[!0-9]*) return 1 ;; esac
    [ "$pid" -gt 1 ] && [ -r "/proc/$pid/stat" ] || return 1
    [ "$(awk '{print $22}' "/proc/$pid/stat" 2>/dev/null)" = "$started" ] || return 1
    printf '%s\n' "$pid"
}

stop_managed() {
    if pid=$(managed_pid); then
        kill "$pid"
        for attempt in 1 2 3 4 5; do
            managed_pid >/dev/null || return 0
            sleep 1
        done
        printf 'redmond-kettle did not stop\n' >&2
        return 1
    fi
}

start_managed() {
    if managed_pid >/dev/null; then return 0; fi
    if [ -e /tmp/ble-temp/key ] || [ -L /tmp/ble-temp/key ]; then
        printf 'Stop the existing foreground kettle listener first\n' >&2
        return 1
    fi
    setsid "$entry" daemon </dev/null >/dev/null 2>&1 9>&- &
    for attempt in 1 2 3 4 5; do
        managed_pid >/dev/null && return 0
        sleep 1
    done
    printf 'redmond-kettle failed to start\n' >&2
    return 1
}

run_managed() {
    mkdir -p "$runtime"
    exec 8>"$runtime/daemon.lock"
    flock -n 8 || exit 0
    fifo=$runtime/output
    rm -f "$fifo"
    mkfifo "$fifo"
    logger -t redmond-kettle < "$fifo" 8>&- &
    log_pid=$!
    "$entry" run > "$fifo" 2>&1 8>&- &
    service_pid=$!
    cleanup_managed() {
        rm -f "$pidfile" "$fifo"
        kill "$log_pid" 2>/dev/null || true
        wait "$log_pid" 2>/dev/null || true
    }
    trap cleanup_managed EXIT
    trap 'kill "$service_pid" 2>/dev/null || true; wait "$service_pid" || true; exit 0' HUP INT TERM
    printf '%s %s\n' "$$" "$(awk '{print $22}' "/proc/$$/stat")" > "$pidfile"
    wait "$service_pid"
}

case "${1:-run}" in
    start|stop|restart)
        mkdir -p "$runtime"
        exec 9>"$runtime/control.lock"
        flock 9
        case "$1" in
            start) start_managed ;;
            stop) stop_managed ;;
            restart) stop_managed && start_managed ;;
        esac
        exit
        ;;
    status)
        if [ -r "$pidfile" ] && pid=$(managed_pid); then
            printf 'redmond-kettle running (PID %s)\n' "$pid"
            exit 0
        fi
        printf 'redmond-kettle stopped\n'
        exit 1
        ;;
    daemon) run_managed; exit ;;
    run) ;;
    *) printf 'Usage: redmond-kettle {start|stop|restart|status|run}\n' >&2; exit 2 ;;
esac
config_dir=${YAM_ETC:-$yam_root/etc}/lynx.redmond-kettle
state_dir=${YAM_VAR:-$yam_root/var}/lynx.redmond-kettle
export YAM_PKG="$pkg" YAM_ETC="${YAM_ETC:-$yam_root/etc}"
"$pkg/bin/redmond-kettle-postinstall" --config-only
KETTLE_ADDRESS=$(cat "$config_dir/device.conf")
export KETTLE_ADDRESS
mkdir -p "$state_dir"
chmod 700 "$state_dir"
export KETTLE_SAVED_KEY="$state_dir/kettle.key"
export KETTLE_PROBE_PREFIX=
export KETTLE_LIBEXEC_DIR=$pkg/share/lynx.redmond-kettle/libexec
export KETTLE_ACTIVITY_PATTERN="${KETTLE_ACTIVITY_PATTERN-$pkg/share/lynx.redmond-kettle/activity.led}"
pending=$state_dir/kettle.key.pending

check_key() {
    [ ! -L "$1" ] && [ -f "$1" ] && [ "$(wc -c < "$1")" -eq 8 ]
}

base=/tmp/ble-temp
if [ ! -d "$base" ]; then
    mkdir -m 700 "$base" || [ -d "$base" ]
fi
test ! -L "$base"
set -- $(ls -ldn "$base")
test "$1" = drwx------
test "$3" = "$(id -u)"
run=$(mktemp -d "$base/redmond-kettle.XXXXXX")
key_path=$base/key
child=
cleanup() {
    if [ "$(readlink "$key_path" 2>/dev/null || true)" = "$run/key" ]; then
        rm -f "$key_path"
    fi
    rm -f "$run/key"
    rmdir "$run"
}
stop() {
    if [ -n "$child" ]; then
        kill "$child" 2>/dev/null || true
        wait "$child" || true
    fi
    exit 0
}
trap cleanup EXIT
trap stop HUP INT TERM
ln -s "$run/key" "$key_path"
export KETTLE_RUN_DIR="$run"

if [ -e "$KETTLE_SAVED_KEY" ] || [ -L "$KETTLE_SAVED_KEY" ]; then
    check_key "$KETTLE_SAVED_KEY"
    chmod 600 "$KETTLE_SAVED_KEY"
    ln -s "$KETTLE_SAVED_KEY" "$run/key"
    rm -f "$pending"
else
    if [ ! -e "$pending" ] && [ ! -L "$pending" ]; then
        candidate_tmp=$(mktemp "$state_dir/pending.XXXXXX")
        if ! head -c 8 /dev/urandom > "$candidate_tmp" || ! ln "$candidate_tmp" "$pending"; then
            rm -f "$candidate_tmp"
            exit 1
        fi
        rm -f "$candidate_tmp"
    fi
    check_key "$pending"
    chmod 600 "$pending"
    export KETTLE_PAIR_KEY="$pending"
fi

while :; do
    "$KETTLE_LIBEXEC_DIR/alice-kettle" --continuous "$pkg/bin/redmond-kettle-worker" \
        "$config_dir/phrases.conf" &
    child=$!
    result=0
    wait "$child" || result=$?
    child=
    [ "$result" = 1 ] || exit "$result"
    sleep 3 &
    child=$!
    wait "$child"
    child=
done
