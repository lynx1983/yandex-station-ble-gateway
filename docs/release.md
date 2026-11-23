# Release checklist

Use this sequence for a YAM package release. The design and build layout are
in [Gateway architecture](architecture.md).

## 1. Prepare the version

1. Choose `X.Y.Z` and write it to `VERSION`.
2. Set the same `version: X.Y.Z` line in `packaging/yam/yam.yaml`. Keep the
   `homepage` and `targets` fields unchanged.
3. Run:

   ```sh
   make check
   make test
   ```

   `check-version.sh` fails when the two files drift or a README hardcodes a
   release number.

4. Commit the version bump as `Release X.Y.Z`.

## 2. Tag and push

Use the `v` prefix, consistent with earlier releases:

```sh
git tag -a vX.Y.Z -m "Release X.Y.Z"
git push origin main
git push origin vX.Y.Z
```

Delete a mistakenly created tag locally and on `origin` before announcing the
release:

```sh
git tag -d X.Y.Z
git push origin :refs/tags/X.Y.Z
```

## 3. Build on the WSL/host build machine

The ARM toolchain, `libbsa.so`, YAM profiles and the musl notice are not in
the repository.

```sh
mkdir -p build/phase1
ssh -F "$HOME/.ssh/config" -T station 'cat /system/workdir/lib/libbsa.so' \
  > build/phase1/libbsa.so

make
make -B build/phase1/alice-kettle ARM_CC=arm-linux-musleabihf-gcc

MUSL_COPYRIGHT=/path/to/musl/COPYRIGHT \
YAM_PROFILES=/path/to/yam/platforms \
make package
```

The expected artifact is
`build/dist/redmond-kettle-X.Y.Z-yandexmini.yampkg`. `pack-yam.sh` derives the
archive name and the staged `yam.yaml` version from `VERSION`; `YAM_PUBLISH`
may override the packer executable.

## 4. Verify and smoke test

- Check that the archive name contains `X.Y.Z`.
- Check that the staged `yam.yaml` keeps `homepage` and the new version.
- Install the package on a Station and confirm:

  ```sh
  /data/yam/bin/redmond-kettle status
  grep redmond-kettle /var/log/messages
  ```

- Ask for the temperature and status, start and stop heating with a filled
  kettle under supervision, and verify the pairing prompt with a key that is
  not yet configured.
- For build and runtime problems, see [Troubleshooting](troubleshooting.md).

## 5. Publish (optional)

With `YAM_AUTHOR_KEY_FILE` and `YAM_PUBLISH_TOKEN` already in the environment:

```sh
yam-publish publish --source packaging/yam --target yandexmini \
  --artifact "build/dist/redmond-kettle-$(cat VERSION)-yandexmini.yampkg"
```

See the [YAM publishing guide](https://github.com/nyakokitsu/yam/blob/main/docs/PUBLISHING.md).

## Build on GitHub Actions

`.github/workflows/package.yml` builds the package when a `v*` tag is
pushed. The tagged commit must contain the workflow, and its `VERSION`
must match the tag without the leading `v`. The job uploads the
`.yampkg` and its SHA-256 checksum to the workflow run.
For a trial build, select **Run workflow** on the Actions page and choose
the default branch. The manual run uses the version in `VERSION`.

The job uses a GitHub-hosted `ubuntu-latest` runner. It installs the ARM
toolchain and downloads pinned musl and YAM inputs. Provide the compatible
Station copy of `libbsa.so` as two repository Actions secrets. The
compressed, Base64-encoded library exceeds the 48 KB limit for one
secret. With an authenticated GitHub CLI, run from the repository root:

```bash
set -o pipefail
bsa_payload=$(gzip -n -9c build/phase1/libbsa.so | base64 -w0) &&
  test "${#bsa_payload}" -gt 40000 &&
  test "${#bsa_payload}" -le 88000 &&
  printf '%s' "${bsa_payload:0:40000}" | gh secret set YAM_BSA_PART_1 &&
  printf '%s' "${bsa_payload:40000}" | gh secret set YAM_BSA_PART_2
unset bsa_payload
```

The workflow restores the library for linking and uploads the package
and checksum. Publishing to YAM Registry or attaching the archive to
a GitHub Release is a separate operation.
