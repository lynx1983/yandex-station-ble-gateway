# Troubleshooting

This page covers current package behavior. For the design, see
[Gateway architecture](architecture.md).

## Alice asks about the kettle but hears nothing

Check that the service is running and watch syslog while repeating the phrase:

```sh
/data/yam/bin/redmond-kettle status
grep redmond-kettle /var/log/messages
```

Logs use the tag `redmond-kettle`; on this firmware `/var/log` points to
`/tmp`, so entries disappear after a reboot. Useful lines are
`Kettle request accepted`, `Kettle job started` / `Kettle job finished`,
`Cancellation sent`, `Temperature read`, `Vocalize directive sent`, and
`Quasar speech state`.

- No `Kettle request accepted`: the phrase did not match. Check
  `/data/yam/etc/lynx.redmond-kettle/phrases.conf` and restart the service.
  The matcher ignores case, an optional `Алиса,` prefix and final punctuation.
- Accepted but no `Vocalize directive sent`: the worker failed or timed out.
  Look for `Kettle job timed out`, `Kettle key missing`, or probe exit codes.
- Directive sent but `Quasar speaking state was not observed`: Quasar did not
  report speech before `KETTLE_SPEECH_TIMEOUT_MS`. The audible result must be
  confirmed on the Station; host tests only check the directive.

## The kettle is not connected / pairing does not finish

Say any kettle command and follow the prompt while the kettle is on its
powered base in standby. Keep the phone disconnected. Hold the power button
until three short signals. The Station authenticates and reads status while
waiting for the physical window.

- `Укажите адрес чайника в настройках службы` — write the BLE address as one
  `AA:BB:CC:DD:EE:FF` line to
  `/data/yam/etc/lynx.redmond-kettle/device.conf` and restart the service.
- `Ключ чайника не настроен` — start voice pairing; a foreground
  `scripts/run-station.sh --pair <key>` session is the terminal fallback.
- `Привязка чайника не завершилась вовремя` — the button window expired.
  Raise `KETTLE_PAIR_TIMEOUT_MS` in the service environment if needed. The
  same value is exported to the worker, so both sides stay in sync.
- `Не удалось привязать чайник` — the key was not verified. Failed attempts
  leave a `.pending` candidate that is reused on the next attempt.

### Pairing log trace

A voice pairing produces this sequence in syslog:

```
Listener timeouts: session_ms=… speech_ms=…
Kettle request accepted: temperature
Starting pairing job after key-missing reply
Kettle job started: operation=pair pid=… timeout_ms=… wrapped=… target=…
pair_worker start timeout_ms=… server_pid=… socket_inode=…
pair_worker status_failed attempt=1 elapsed_s=0
pair_worker status_ok attempt=… elapsed_s=…
pair_worker verified attempts=… elapsed_s=…
Kettle job finished: operation=pair pid=… exit=0 elapsed_ms=… stale=0 timed_out=0
Voice pairing verified
Pairing verified; waiting for a fresh command
```

`pair_worker status_failed` lines are throttled after the first two attempts,
so a long wait stays readable. The meaning of the exit events:

- `pair_worker timeout` — the button window expired. Compare `timeout_ms` with
  `KETTLE_PAIR_TIMEOUT_MS`; both sides use the same resolved value.
- `pair_worker server_changed` / `socket_changed` — BSA or its socket was
  replaced during the wait. The worker aborts rather than touching another
  Bluetooth session; check what restarted `bsa_server`.
- `pair_worker key_save_failed` — status succeeded but the verified key could
  not be written. Check permissions on `/data/yam/var/lynx.redmond-kettle`.
- `Kettle job finished` with `timed_out=1` or `exit=-1` — the client killed the
  worker; look for the preceding `Stopping kettle job … with SIGTERM/SIGKILL`
  line.

The verified key is `/data/yam/var/lynx.redmond-kettle/kettle.key` with mode
`0600`. Move this file and `kettle.key.pending` aside before pairing a
different kettle.

## GATT read fails or BSA stops

The worker tries a direct GATT connection first and performs at most one
fallback scan. It checks that `bsa_server`'s PID and the `/tmp/bt-daemon-socket`
inode are unchanged before and after that scan; if either changed, it aborts
instead of touching another Bluetooth session.

- `GATT open failed` followed by a fallback scan is expected when the cache
  entry is stale. This firmware needs a nonempty GATT cache image; an empty
  reply previously stopped BSA.
- `Kettle absent from fallback scan` — compare scans with the kettle off and
  on its base, and check that the advertised name is `RFS-KKL003`.
- `Kettle probe exit=N` — inspect the probe log in the temporary worker
  directory and the BSA process state. Do not restart BSA manually while the
  service is running.

## Build or package failures

- `Copy libbsa.so from your Station into build/work/libbsa.so before building
  the probes` — copy the library from your own Station into `build/work/`
  before `make`. The repository does not ship it.
- `Build alice-kettle with a static ARM musl compiler before packaging` —
  rebuild with
  `make -B build/work/alice-kettle ARM_CC=arm-linux-musleabihf-gcc`.
- `YAM_PROFILES` / `MUSL_COPYRIGHT` errors — point them at the upstream YAM
  platforms directory and the `COPYRIGHT` file matching the musl build.
- `version ... is inconsistent` — update both `VERSION` and the `version:`
  line in `packaging/yam/yam.yaml`, then run `make check`.

## Service management

```sh
/data/yam/bin/redmond-kettle status
/data/yam/bin/redmond-kettle restart
/data/yam/bin/redmond-kettle stop
```

Stop the service before removing the package. YAM v2 has no uninstall hook;
the init entry remains and exits immediately when the package is absent. To
disable boot startup while keeping the package, move
`/etc/init.d/S99redmond-kettle` outside the `S??*` startup pattern.
