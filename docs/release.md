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

Register a dedicated Linux self-hosted runner with the `yam-build` label
and prepare GNU Make, Python 3, a native C compiler, ARM hard-float
binutils, an ARM musl cross-compiler and the official YAM packer. Set these
repository Actions variables to paths readable by the runner account:

| Variable | Value |
| --- | --- |
| `YAM_BSA_LIB` | Absolute path to the compatible Station copy of `libbsa.so`. |
| `YAM_MUSL_COPYRIGHT` | Absolute path to the matching musl `COPYRIGHT`. |
| `YAM_PROFILES` | Absolute path to the YAM profiles directory. |
| `YAM_ARM_CC` | Compiler command, defaults to `arm-linux-musleabihf-gcc`. |
| `YAM_ARM_CC_FLAGS` | Extra compiler flags, if needed by the toolchain. |
| `YAM_PUBLISH` | Packer executable, defaults to `yam-publish`. |

Keep `libbsa.so` outside the checkout. Run the packaging job only for
trusted tags on the dedicated runner. The job needs read-only GitHub
permissions. Publishing the resulting archive to YAM Registry or
attaching it to a GitHub Release is a separate operation.
