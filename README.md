# Yandex Station BLE Gateway

[Русская версия](README.ru.md)

Local voice control for a Redmond SkyKettle on the first-generation
Yandex Station Mini (YNDX-0004). The verified kettle advertised as
`RFS-KKL003`. The Station uses its built-in Bluetooth controller,
stock Broadcom BSA, and Quasar recognition and speech.

## Voice commands

| Say after “Алиса” | Result |
| --- | --- |
| «Какая температура воды в чайнике?» | Read and speak the water temperature. |
| «Какой статус чайника?» or «Как там чайник?» | Read and speak the heating state, approximate temperature, and target while heating. |
| «Вскипяти воду в чайнике» | Start boiling from idle and confirm the running state. |
| «Нагрей воду в чайнике до 65 градусов» | Start the native heat program from idle with a target of 35–90 °C. |
| «Прекрати нагрев чайника» | Stop heating and confirm it, or report that heating is off. |

Direct GATT access is attempted first, with one scan and retry after an
open failure. Use the known nonempty cache image for this firmware.
An empty reply previously stopped BSA.

## Build on WSL

Prerequisites are GNU Make, Python 3, an ARM hard-float GCC toolchain
with static libc, ARM binutils, and a root SSH target for the Station.
Copy the matching library from your own Station into the ignored build directory, then build. The repository does not distribute this firmware library.

```sh
mkdir -p build/phase1
ssh -F "$HOME/.ssh/config" -T station 'cat /system/workdir/lib/libbsa.so' > build/phase1/libbsa.so
make
```

`ARM_CC` and `ARM_CC_FLAGS` can select a locally extracted cross
compiler and its sysroot. `CROSS_COMPILE` selects the binutils prefix.

`make check` validates version references, the Python scripts, shell
syntax, and the C client without the ARM toolchain or the BSA library.
`make test` compiles the same client for the host and runs the fake-Quasar
regression suite for recognition, cancellation, timeouts, and pairing.
`VERSION` is the single source of truth for the package version;
`pack-yam.sh` derives the archive name and the staged `yam.yaml` from it.
To cut a release, update `VERSION` and the matching `version:` line in
`packaging/yam/yam.yaml`, then run `make check`.

## Run one session

```sh
KETTLE_ADDRESS=AA:BB:CC:DD:EE:FF scripts/run-station.sh /absolute/path/to/kettle.key
```

The launcher defaults to SSH alias `station` and accepts another target
as a second argument. `SSH_CONFIG` selects a different SSH config file.
Omitting the key path selects the local `build/phase1/wsl-kettle.key`.
The launcher listens for one command and removes its temporary files
from the Station when the session ends. Start a new session for each
additional command.

## Pair a new key by voice

Start a normal session with a path that does not yet contain a key:

```sh
KETTLE_ADDRESS=AA:BB:CC:DD:EE:FF scripts/run-station.sh /absolute/path/to/kettle.key
```

Say any configured kettle command. Alice asks you to hold the kettle's
power button until three short signals. Leave the kettle on its powered
base in standby and keep the phone disconnected. The Station attempts
authentication and a read-only status query while waiting for the
physical pairing window.

After confirming the pairing aloud, the listener waits for a fresh
command in the same session. The command that triggered pairing is
discarded. At session exit WSL saves the verified eight-byte key to
the requested path with mode 0600. Failed attempts retain a private
`.pending` candidate for retry. An existing key is not replaced.
Put keys in an ignored or private location.

For a terminal-only fallback, use
`scripts/run-station.sh --pair /absolute/path/to/kettle.key`.

## Customize voice phrases

Edit [config/kettle-phrases.conf](config/kettle-phrases.conf) or set
`KETTLE_PHRASES` to another UTF-8 file. Each line maps one phrase to
one operation. Repeat the operation to add more phrases:

```ini
temperature=какая температура воды в чайнике
temperature=сколько градусов в чайнике
status=какой статус чайника
status=как там чайник
stop=прекрати нагрев чайника
boil=вскипяти воду в чайнике
heat=нагрей воду в чайнике до
heat.75=нагрей воду в чайнике для термоса
```

The matcher ignores case, an optional Alice prefix and final
punctuation. It uses full-phrase matches. Invalid entries and
normalized duplicates cause a startup error. Changes take effect on
the next session. Boil testing requires a filled kettle and supervision.

See [the Bluetooth investigation](docs/bluetooth-stack.md) for the
firmware-specific transport.

A `heat.<degrees>=<phrase>` entry matches the whole phrase and uses the
configured target (35–90 °C). The default thermos phrase uses 75 °C.
When upgrading an older configuration, the built-in thermos phrase
applies until the file defines a fixed-temperature alias. Edit or add
entries and restart the service to apply changes.

## Install with Alice Homebrew

Use the local package upload in Alice Homebrew and select the generated
`redmond-kettle-<version>-yandexmini.yampkg` archive (`<version>` comes from
the `VERSION` file). This build is for the first-generation
Yandex Station Mini (YNDX-0004) and the Redmond SkyKettle RK-G214S.
The profile is `yandexmini`; the probes use the Station's existing
ARM hard-float loader and BSA library.

Stop any existing foreground session with Ctrl+C before uploading the
upgrade. On the Station, the install hook starts the service in the
background and installs `/etc/init.d/S99redmond-kettle` for boot. On a clean
installation, put the kettle's BLE address (one line, `AA:BB:CC:DD:EE:FF`
format) in `/data/yam/etc/lynx.redmond-kettle/device.conf`, then restart
the service. Ask Alice for the kettle temperature, follow the pairing
prompt, and repeat the original command.

An upgrade from 1.1.x also needs this one-time address setup. The saved
key still works when selecting the same kettle. Before selecting a
different kettle, stop the service and move the old `kettle.key` and
`kettle.key.pending` files out of the state directory, then restart.

To identify the address, run the packaged scanner from WSL and look for
`RFS-KKL003` in the decoder output. If multiple devices use this name,
compare scans with your kettle off and on its base.

```sh
mkdir -p build
ssh station 'cd /tmp && LD_PRELOAD=libpthread.so.0 LD_LIBRARY_PATH=/system/workdir/lib /data/yam/share/lynx.redmond-kettle/libexec/discovery' > build/scan.bin
python3 scripts/decode-bsa-discovery.py build/scan.bin
```

Use the Station shell to manage the service and read its logs:

```sh
/data/yam/bin/redmond-kettle status
/data/yam/bin/redmond-kettle restart
grep redmond-kettle /var/log/messages
```

Logs use the Station syslog with the tag `redmond-kettle`. On this firmware
`/var/log` points to `/tmp`, so logs are cleared on reboot.
Run `/data/yam/bin/redmond-kettle stop` before removing the package. YAM v2
has no uninstall hook, so the init entry remains and exits immediately
when the package is absent. To disable boot startup while keeping the
package, move `/etc/init.d/S99redmond-kettle` outside the `S??*` startup
pattern. Reinstalling the package restores the entry.

- Phrases: `/data/yam/etc/lynx.redmond-kettle/phrases.conf`. Edit the aliases
  and restart the session to reload them. Reinstallation preserves the file.
- Verified key: `/data/yam/var/lynx.redmond-kettle/kettle.key` (mode `0600`).
  The adjacent `.pending` file retains the candidate across failed pairing
  attempts. The next service start clears it after a key has been verified.

Build the BSA probes with the Station-compatible ARM hard-float
cross-toolchain. Build the voice listener with an ARM hard-float musl
cross-compiler and use the official YAM publisher:

```sh
make all
make -B build/phase1/alice-kettle ARM_CC=arm-linux-musleabihf-gcc
MUSL_COPYRIGHT=/path/to/musl/COPYRIGHT \
  YAM_PROFILES=/path/to/yam/platforms scripts/pack-yam.sh
```

Set `YAM_PUBLISH` to the packer executable if it is outside `PATH`.
Use the `COPYRIGHT` file that matches the musl version used for the build.

The package displays a brighter pink pulse while a kettle command is
running and Quasar is idle. It releases the indicator before speaking.
After stopping the background service, use `KETTLE_ACTIVITY_PATTERN= /data/yam/bin/redmond-kettle run`
to disable the pattern for that session.

## Registry publication

The current package identity is `lynx/redmond-kettle` and the target is
`yandexmini`. The package contains the project MIT license and the
notices for its statically linked musl runtime. The package uses the
Station's BSA library in place.

Publish the generated archive with the official publisher with `YAM_AUTHOR_KEY_FILE` and `YAM_PUBLISH_TOKEN`
already in the host environment:

```sh
yam-publish publish --source packaging/yam --target yandexmini \
  --artifact "build/dist/redmond-kettle-$(cat VERSION)-yandexmini.yampkg"
```

See the [YAM publishing guide](https://github.com/nyakokitsu/yam/blob/main/docs/PUBLISHING.md).
