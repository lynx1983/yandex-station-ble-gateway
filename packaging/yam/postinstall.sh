#!/bin/sh
set -eu
umask 077

require_commands() {
    for command in "$@"; do
        command -v "$command" >/dev/null 2>&1 || {
            printf 'redmond-kettle postinstall: missing required command: %s\n' \
                "$command" >&2
            exit 1
        }
    done
}
require_commands mkdir cat grep awk mktemp mv cmp chmod

: "${YAM_PKG:?}"
if [ ! -d "$YAM_PKG" ]; then
    printf 'YAM_PKG is not a directory: %s\n' "$YAM_PKG" >&2
    exit 1
fi
for payload in bin/redmond-kettle share/lynx.redmond-kettle/phrases.conf \
        share/lynx.redmond-kettle/boot.sh; do
    if [ ! -r "$YAM_PKG/$payload" ] || [ -L "$YAM_PKG/$payload" ]; then
        printf 'Missing or unsafe package payload: %s\n' "$YAM_PKG/$payload" >&2
        exit 1
    fi
done
[ -x "$YAM_PKG/bin/redmond-kettle" ] || {
    printf 'Package service is not executable: %s/bin/redmond-kettle\n' "$YAM_PKG" >&2
    exit 1
}

config_dir=${YAM_ETC:-/data/yam/etc}/lynx.redmond-kettle
mkdir -p "$config_dir"
phrases=$config_dir/phrases.conf
if [ "${1:-}" != --config-only ] && [ ! -e "$phrases" ] && [ ! -L "$phrases" ]; then
    (set -C; cat "$YAM_PKG/share/lynx.redmond-kettle/phrases.conf" > "$phrases")
fi

device=$config_dir/device.conf
if [ ! -e "$device" ] && [ ! -L "$device" ]; then
    (set -C; : > "$device")
fi

[ "${1:-}" != --config-only ] || exit 0

# Migrate stock phrases while preserving custom entries.
old='heat.75=нагрей воду чайнике для термоса'
corrected='heat.75=нагрей воду в чайнике для термоса'
status_alias='status=как там чайник'
if [ ! -L "$phrases" ]; then
    remove_typo=0
    add_status=0
    if grep -Fxq "$old" "$phrases" && grep -Fxq "$corrected" "$phrases"; then
        remove_typo=1
    fi
    if grep -Fxq 'status=какой статус чайника' "$phrases" &&
        ! grep -Eiq '=[[:space:]]*(алиса[, ]+[[:space:]]*)?как там чайник[[:space:]?.!]*$' "$phrases"; then
        add_status=1
    fi
    if [ "$remove_typo" = 1 ] || [ "$add_status" = 1 ]; then
        updated=$(mktemp "$config_dir/.phrases.XXXXXX")
        trap 'rm -f "$updated"' EXIT
        awk -v old="$old" -v remove="$remove_typo" -v status="$status_alias" -v add="$add_status" '
            !(remove && $0 == old)
            END { if (add) print status }
        ' "$phrases" > "$updated"
        mv -f "$updated" "$phrases"
        trap - EXIT
    fi
fi
case "${YAM_ROOT:-}" in
    /data/yam|/bkupgrade/data/yam) ;;
    *) exit 0 ;;
esac
[ "${YAM_PLATFORM:-}" = yandexmini ] || exit 0

hook=/etc/init.d/S99redmond-kettle
marker='# Managed by the lynx/redmond-kettle YAM package.'
if [ -e "$hook" ] || [ -L "$hook" ]; then
    if [ -L "$hook" ] || ! grep -Fxq "$marker" "$hook"; then
        printf 'Refusing to replace %s\n' "$hook" >&2
        exit 1
    fi
fi
boot="$YAM_PKG/share/lynx.redmond-kettle/boot.sh"
if ! cmp -s "$boot" "$hook" || [ ! -x "$hook" ]; then
    mount_mode=$(awk '$2 == "/" {split($4, opts, ","); print opts[1]; exit}' /proc/mounts)
    temp=
    restore_mount=0
    cleanup() {
        [ -z "$temp" ] || rm -f "$temp"
        [ "$restore_mount" = 0 ] || mount -o remount,ro /
    }
    trap cleanup EXIT
    trap 'exit 1' HUP INT TERM
    case "$mount_mode" in
        ro) mount -o remount,rw /; restore_mount=1 ;;
        rw) ;;
        *) exit 1 ;;
    esac
    temp=$(mktemp /etc/init.d/.redmond-kettle.XXXXXX)
    cat "$boot" > "$temp"
    chmod 755 "$temp"
    mv -f "$temp" "$hook"
    temp=
    cleanup
    trap - EXIT HUP INT TERM
fi

# Launch from the package directory supplied by YAM. The detached process
# owns its own stdio so it cannot keep the install transaction open.
"$YAM_PKG/bin/redmond-kettle" restart
