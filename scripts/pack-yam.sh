#!/bin/sh
# Assemble the built YNDX-0004 payload with the official YAM packer.
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
: "${YAM_PROFILES:?Set YAM_PROFILES to the upstream YAM platforms directory}"
: "${MUSL_COPYRIGHT:?Set MUSL_COPYRIGHT to the COPYRIGHT file from the musl build}"
voice="$root/build/phase1/alice-kettle"
if ! readelf -sW "$voice" | awk '$NF == "__init_libc" {musl=1} END {exit !musl}'; then
    printf 'Build alice-kettle with a static ARM musl compiler before packaging\n' >&2
    exit 1
fi
output=${1:-$root/build/dist/redmond-kettle-1.2.5-yandexmini.yampkg}
mkdir -p "$(dirname -- "$output")"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
trap 'exit 1' HUP INT TERM
install -m 644 "$root/packaging/yam/yam.yaml" "$root/LICENSE" "$stage/"
install -m 644 "$MUSL_COPYRIGHT" "$stage/musl-COPYRIGHT"
install -m 755 "$root/packaging/yam/"*.sh "$stage/"
mkdir -p "$stage/libexec"
for binary in alice-kettle discovery gatt-temperature gatt-stop gatt-boil; do
    install -m 755 "$root/build/phase1/$binary" "$stage/libexec/$binary"
done
install -m 755 "$root/scripts/station-temperature-worker.sh" "$stage/redmond-kettle-worker"
install -m 644 "$root/config/kettle-phrases.conf" "$stage/phrases.conf"
# Run a finite pink pulse so the hardware goes dark even if the helper
# outlives its owner. Start bright so short commands are visible.
awk 'BEGIN {
    for (frame = 0; frame < 630; frame++) {
        phase = (frame % 20) / 20
        brightness = 0.60 + 0.40 * cos(2 * atan2(0, -1) * phase)
        rgb = sprintf("%02X%02X%02X", 255 * brightness, 32 * brightness, 139 * brightness)
        printf "%s %s %s %s %s %s 70\n", rgb, rgb, rgb, rgb, rgb, rgb
    }
    print "000000 000000 000000 000000 000000 000000 100"
}' > "$stage/activity.led"
"${YAM_PUBLISH:-yam-publish}" pack --source "$stage" --platform yandexmini \
    --profiles "$YAM_PROFILES" --output "$output"
