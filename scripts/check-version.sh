#!/bin/sh
# Fail when a checked-in package version drifts from the VERSION file.
set -eu
root=${CHECK_VERSION_ROOT:-$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)}
version=$(cat "$root/VERSION")
fail=0

if ! printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    printf 'VERSION: expected MAJOR.MINOR.PATCH, got %s\n' "$version" >&2
    exit 1
fi

if ! grep -Fqx "version: $version" "$root/packaging/yam/yam.yaml"; then
    printf 'packaging/yam/yam.yaml: expected "version: %s"\n' "$version" >&2
    fail=1
fi

for file in README.md README.ru.md; do
    if grep -Eq 'redmond-kettle-[0-9]+\.[0-9]+' "$root/$file"; then
        printf '%s: hardcodes a package version; use <version> and VERSION\n' "$file" >&2
        fail=1
    fi
done

[ "$fail" = 0 ] || exit 1
printf 'version %s is consistent\n' "$version"
