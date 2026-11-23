#!/bin/sh
# Negative tests for the version consistency checker.
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
trap 'exit 1' HUP INT TERM

mkdir -p "$tmp/packaging/yam"
printf '1.2.3\n' > "$tmp/VERSION"
printf 'version: 1.2.3\n' > "$tmp/packaging/yam/yam.yaml"
: > "$tmp/README.md"
: > "$tmp/README.ru.md"

expect_ok() {
    if ! CHECK_VERSION_ROOT="$tmp" "$root/scripts/check-version.sh" >/dev/null 2>&1; then
        printf 'check-version rejected a consistent tree\n' >&2
        exit 1
    fi
}

expect_fail() {
    if CHECK_VERSION_ROOT="$tmp" "$root/scripts/check-version.sh" >/dev/null 2>&1; then
        printf 'check-version accepted an inconsistent tree\n' >&2
        exit 1
    fi
}

expect_ok

printf 'version: 1.2.4\n' > "$tmp/packaging/yam/yam.yaml"
expect_fail
printf 'version: 1.2.3\n' > "$tmp/packaging/yam/yam.yaml"

printf 'v1.2.3\n' > "$tmp/VERSION"
expect_fail
printf '1.2.3\n' > "$tmp/VERSION"

printf 'redmond-kettle-1.2.3-yandexmini.yampkg\n' > "$tmp/README.md"
expect_fail

printf 'check-version negative tests passed\n'
