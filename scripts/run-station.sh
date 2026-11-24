#!/usr/bin/env bash
# Run one temporary Station voice session or enroll a Ready for Sky key.
set -euo pipefail

require_commands() {
    local cmd
    for cmd in "$@"; do
        command -v "$cmd" >/dev/null 2>&1 || {
            printf 'run-station: missing required command: %s\n' "$cmd" >&2
            exit 2
        }
    done
}
require_commands wc mktemp install head chmod mkdir rm rmdir tar ssh

root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build="$root/build/work"
mode=voice
if [[ ${1:-} == --pair ]]; then
    mode=pair
    shift
fi
if (( $# > 2 )) || [[ ${1:-} == -* ]]; then
    printf 'Usage: %s [--pair] [KEY_FILE [SSH_TARGET]]\n' "$0" >&2
    exit 2
fi
key=${1:-$build/wsl-kettle.key}
# Keep the path absolute without realpath -m, which BSD realpath lacks.
[[ $key == /* ]] || key=$PWD/$key
target=${2:-station}
pending=
phrases=${KETTLE_PHRASES:-$root/config/kettle-phrases.conf}
: "${KETTLE_ADDRESS:?Set KETTLE_ADDRESS to the target kettle BLE address}"

for name in alice-kettle discovery gatt-temperature gatt-stop gatt-boil; do
    if [[ ! -f $build/$name || ! -x $build/$name ]]; then
        printf 'Missing build artifact: %s (run make first)\n' "$build/$name" >&2
        exit 2
    fi
done
for input in "$root/scripts/station-temperature-worker.sh" "$phrases"; do
    if [[ ! -f $input || ! -r $input ]]; then
        printf 'Missing input file: %s\n' "$input" >&2
        exit 2
    fi
done

valid_key() {
    [[ -f $1 ]] && [[ $(wc -c < "$1") -eq 8 ]]
}
if [[ -e $key ]] && ! valid_key "$key"; then
    printf 'Key file must contain exactly eight bytes: %s\n' "$key" >&2
    exit 2
fi
if [[ $mode == pair && -e $key ]]; then
    printf 'Key already exists: %s\n' "$key" >&2
    exit 2
fi

umask 077
stage=$(mktemp -d)
files=(alice-kettle discovery gatt-temperature gatt-stop gatt-boil worker phrases.conf address)
cleanup() {
    for name in "${files[@]}" key pair candidate; do
        rm -f -- "$stage/$name"
    done
    rmdir -- "$stage"
}
trap cleanup EXIT

for name in alice-kettle discovery gatt-temperature gatt-stop gatt-boil; do
    install -m 700 "$build/$name" "$stage/$name"
done
install -m 700 "$root/scripts/station-temperature-worker.sh" "$stage/worker"
install -m 600 "$phrases" "$stage/phrases.conf"
printf '%s\n' "$KETTLE_ADDRESS" > "$stage/address"
if [[ ! -e $key ]]; then
    pending="$key.pending"
    mkdir -p -- "$(dirname -- "$key")"
    if [[ ! -e $pending && ! -L $pending ]]; then
        (set -o noclobber; head -c 8 /dev/urandom > "$pending")
    fi
    if [[ -L $pending ]] || ! valid_key "$pending"; then
        printf 'Invalid pending key file: %s\n' "$pending" >&2
        exit 2
    fi
    chmod 600 -- "$pending"
    if [[ $mode == pair ]]; then
        install -m 600 "$pending" "$stage/key"
        touch "$stage/pair"
        files+=(key pair)
        printf '%s\n' 'Оставьте чайник на подставке в режиме ожидания.' \
            'Удерживайте кнопку питания до трёх коротких сигналов и сразу нажмите Enter.' >&2
        read -r
    else
        install -m 600 "$pending" "$stage/candidate"
        files+=(candidate)
    fi
elif [[ -f $key ]]; then
    install -m 600 "$key" "$stage/key"
    files+=(key)
fi

remote_status=0
tar -C "$stage" -cf - "${files[@]}" |
    ssh -F "${SSH_CONFIG:-$HOME/.ssh/config}" -o BatchMode=yes -o ConnectTimeout=5 -T -- "$target" '
set -eu
umask 077
work=$(mktemp -d /tmp/alice-kettle.XXXXXX)
key_path=/tmp/ble-temp/key
key_owned=0
cleanup() {
    if [ "$key_owned" = 1 ] || [ "$key_path" -ef "$work/candidate" ]; then
        rm -f "$key_path"
    fi
    for name in alice-kettle discovery gatt-temperature gatt-stop gatt-boil worker phrases.conf address key pair candidate pair-verified; do
        rm -f "$work/$name"
    done
    rmdir "$work"
}
trap cleanup EXIT
trap "exit 143" HUP INT TERM
tar -xf - -C "$work"
KETTLE_ADDRESS=$(cat "$work/address")
export KETTLE_ADDRESS
if [ -f "$work/key" ] || [ -f "$work/candidate" ]; then
    if [ ! -d /tmp/ble-temp ]; then mkdir -m 700 /tmp/ble-temp; fi
    test ! -L /tmp/ble-temp
    set -- $(ls -ldn /tmp/ble-temp)
    test "$1" = "drwx------"
    test "$3" = 0
fi
if [ -f "$work/key" ]; then
    ln "$work/key" "$key_path"
    key_owned=1
else
    export KETTLE_KEY_AVAILABLE=0
    export KETTLE_PAIR_KEY="$work/candidate"
fi
if [ -f "$work/pair" ]; then
    state=$("$work/worker" status)
    printf "Key verified (heating flag and temperature): %s\n" "$state"
else
    status=0
    "$work/alice-kettle" "$work/worker" "$work/phrases.conf" || status=$?
    if [ -f "$work/pair-verified" ]; then exit 21; fi
    exit "$status"
fi
' || remote_status=$?
if [[ $mode == pair && $remote_status == 0 ]] || [[ -n $pending && $remote_status == 21 ]]; then
    ln -- "$pending" "$key"
    rm -- "$pending"
    printf 'Ключ подтверждён и сохранён в %s\n' "$key"
fi
if (( remote_status != 21 )) || [[ -z $pending ]]; then
    exit "$remote_status"
fi
