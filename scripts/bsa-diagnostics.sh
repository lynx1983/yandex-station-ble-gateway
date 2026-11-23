#!/bin/sh
# Stream this script to the Station with SSH. Output goes to stdout.
LC_ALL=C
export LC_ALL

# This script is streamed to a minimal firmware, so report missing tools
# instead of aborting and losing the remaining diagnostics.
missing=
for cmd in uname id cat readlink tr awk sort find grep; do
    command -v "$cmd" >/dev/null 2>&1 || missing="$missing $cmd"
done
if [ -n "$missing" ]; then
    printf 'Diagnostics: unavailable commands:%s\n' "$missing" >&2
fi
if [ ! -d /proc ]; then
    printf 'Diagnostics: /proc is unavailable\n' >&2
    exit 1
fi

printf '\n## Kernel and identity\n'
uname -a
id

printf '\n## Bluetooth and Quasar processes\n'
for proc in /proc/[0-9]*; do
    name=$(cat "$proc/comm" 2>/dev/null) || continue
    case "$name" in
        *bsa*|*bluetooth*|*quasar*|maind|*bluealsa*|*hciattach*)
            printf '\nPID %s (%s)\n' "${proc##*/}" "$name"
            printf 'Executable: '
            readlink "$proc/exe"
            printf 'Arguments: '
            tr '\000' ' ' < "$proc/cmdline"
            printf '\nSocket and controller descriptors:\n'
            for fd in "$proc"/fd/*; do
                target=$(readlink "$fd" 2>/dev/null) || continue
                case "$target" in
                    socket:*|/dev/tty*) printf '%s -> %s\n' "${fd##*/}" "$target" ;;
                esac
            done
            printf 'Bluetooth-related mapped files:\n'
            awk 'tolower($0) ~ /\/[^ ]*(bsa|bluetooth|bluez|btclient)[^ ]*/ && NF >= 6 {print $6}' \
                "$proc/maps" | sort -u
            ;;
    esac
done

printf '\n## Socket tables\n'
for table in unix tcp tcp6; do
    path="/proc/net/$table"
    if [ -r "$path" ]; then
        printf '\n%s\n' "$path"
        cat "$path"
    fi
done

printf '\n## BSA binaries, libraries, and SDK candidates\n'
for root in /system /usr /lib /opt /etc/bluetooth; do
    [ -d "$root" ] || continue
    find "$root" -xdev \( -iname '*bsa*' -o -iname '*broadcom*' \
        -o -iname '*bluetooth*' -o -iname '*bcm43430*' \
        -o -iname '*btclient*' -o -iname 'app_ble*' \) -print
done

printf '\n## References in startup files\n'
for path in /etc/inittab /etc/rc.local /etc/init.d/* /system/etc/init.d/*; do
    [ -f "$path" ] || continue
    grep -nH -E 'bsa_server|BCM43430|/dev/ttyS1' "$path"
    case $? in
        0|1) ;;
        *) printf 'Could not inspect %s\n' "$path" >&2 ;;
    esac
done
