# Gateway architecture

Status: the voice, BLE, and YAM package paths below are implemented. This page
describes the current design; the original measurements and feasibility notes
are in [Investigation history](investigation-history.md). See
[README](../README.md) for install and usage.

## Components

| Part | Source | Responsibility |
| --- | --- | --- |
| Quasar codec | `src/quasar.c` | Decode recognition frames, deduplicate request IDs in `voice.c`, send cancellation and vocalize directives. |
| Phrase matcher | `src/phrases.c` | Normalize text, match aliases, parse target temperatures from 35 to 90 °C. |
| Voice state | `src/voice.c` | Arm on a fresh recognition cycle, reject repeated final IDs and stale replies. |
| Job runner | `src/job.c` | Fork workers in their own process group, apply timeouts, translate output into spoken replies. |
| Activity LED | `src/activity.c` | Run the optional Quasar LED pattern while a worker is active. |
| Listener | `src/listener.c` | Main loop over the loopback socket, job pipes and deadlines. |
| Entry point | `src/alice-kettle.c` | Parse arguments, load aliases, connect to `127.0.0.1:9875`. |
| Station worker | `scripts/station-temperature-worker.sh` | Validate configuration, pair a key or run a probe with one fallback scan. |
| BSA probes | `scripts/bsa-management-probe.S`, `scripts/bsa-gatt-search.inc` | ARM32 clients of the Station's `libbsa.so` for discovery and GATT operations. |
| Service wrapper | `packaging/yam/service.sh` | `daemon`, `run`, `start`, `stop`, `restart`, `status` with a lock and pidfile. |

## Request lifecycle

1. Alice recognition arrives on the loopback connection as a final
   AliceState result. `quasar.c` decodes it and `voice.c` admits it once per
   request ID.
2. The listener sends dialog cancellation and TTS stop, then starts a worker
   through `job.c`. A new recognition cycle marks a pending job stale so its
   reply is discarded.
3. `service.sh` reads `/data/yam/etc/lynx.redmond-kettle/device.conf` and
   exports `KETTLE_ADDRESS`; the worker checks the saved key. It tries a
   direct GATT connection; an open failure allows one scan and retry after
   checking that BSA's PID and listener inode are unchanged.
4. The probe writes a single decoded value to stdout. `job.c` maps exit codes
   and output to a spoken reply: missing key, missing address, busy, started,
   stopped, reached temperature, or a read error.
5. The listener sends START_VOCALIZE and keeps the socket open until Quasar
   reports speaking or the speech deadline expires. A successful pairing
   re-arms the listener for a fresh command in the same session.

## Timeout configuration

All values are milliseconds and can be set in the service environment.
Invalid, zero, or excessively large values fall back to the defaults compiled
into `src/config.h`. Compile-time `-D` overrides remain available for tests and
custom builds.

| Variable | Default | Used for |
| --- | --- | --- |
| `KETTLE_JOB_TIMEOUT_MS` | 60000 | Worker jobs other than pairing. |
| `KETTLE_PAIR_TIMEOUT_MS` | 75000 | Physical pairing window. The client exports the resolved value to the worker and adds `KETTLE_STOP_GRACE_MS` before giving up. |
| `KETTLE_STOP_GRACE_MS` | 1000 | Wait after SIGTERM before SIGKILL. |
| `KETTLE_SESSION_TIMEOUT_MS` | 90000 | One-shot wait for a recognition event. |
| `KETTLE_SPEECH_TIMEOUT_MS` | 8000 | Wait for a Quasar speaking transition after a reply. |

The worker converts `KETTLE_PAIR_TIMEOUT_MS` to whole seconds with `ceil`, so
one configured value drives both sides of the pairing flow.

## Build and verification

```sh
make check   # version, Python, shell, and C syntax; no ARM toolchain needed
make test    # module unit tests plus fake-Quasar integration tests
make         # ARM32 BSA probes and the voice client
make package # stage and pack a YAM archive
```

`src/*.c` is compiled together for ARM and for the host; `VERSION` is the only
package version source. The voice client needs a static ARM hard-float musl
build for packaging, while the probes use Station-compatible glibc and
`libbsa.so`. See [Release checklist](release.md) for the full sequence.

## Operational notes

The packaged service runs as `daemon` with `/data/yam/var/lynx.redmond-kettle`
as its state directory. Mutable phrases and the verified key live there; the
key is mode `0600`. Syslog messages use the tag `redmond-kettle`. On this
firmware `/var/log` points to `/tmp`, so logs do not survive a reboot.

For symptom-based guidance, see [Troubleshooting](troubleshooting.md).

## Reference documents

- [Investigation history](investigation-history.md) — original feasibility notes and live measurements.
- [Bluetooth stack investigation](bluetooth-stack.md) — BSA transport evidence.
- [Redmond protocol investigation](redmond-protocol.md) — GATT handles and Ready for Sky frames.
- [Release checklist](release.md) — versioning, tests, tags, package build.
