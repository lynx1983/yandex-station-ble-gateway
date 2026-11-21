# Gateway architecture

Transport design depends on the Phase 1 BSA investigation.

## Alice voice input feasibility

At revision `b4a1785`, [Soloist's bridge](https://github.com/Horohi/soloist-station-lite/blob/b4a1785/src/soloist-bridge.c#L2783)
connects as a separate TCP client to Quasar `aliced` on
`127.0.0.1:9875`. Its
[voice lifecycle documentation](https://github.com/Horohi/soloist-station-lite/blob/b4a1785/docs/VOICE_REQUESTS.md)
describes admitting explicitly final AliceState recognition results
and deduplicating by request ID. This provides a candidate input path.

The earlier YNDX-0004 SSH snapshot independently found a listener on
`127.0.0.1:9875` (inode 19500, FD 36 of `maind --platformd`, PID
3243 at the time). The target's framing and final recognition events
have yet to be validated.

1. Observe one spoken kettle-specific phrase via a short-lived client.
   Record only matching diagnostics and leave Quasar's dialogue and TTS
   handling untouched.
2. Add a dry-run mapping for the final phrase, with duplicate request
   suppression and no BLE actions.
3. Independently verify BSA GATT and the Ready for Sky protocol with a
   state or temperature query against the identified kettle.
4. Join the voice adapter and kettle interface once both work. Heating
   commands require separate approval and supervised testing.

The installed Soloist bridge had an established TCP connection to
`127.0.0.1:9875`. A bounded, read-only check of new entries in its
existing log matched the kettle-temperature phrase. This confirms that
the installed bridge sees the target phrase on this Station. The check
did not capture raw AliceState frames, verify duplicate handling, or
invoke BLE. No changes to the installed bridge were needed.

### Voice dry run

The original ARM32 relay opens the loopback
socket, copies received bytes to stdout, and exits after 45 seconds.
The original host parser reads the frames
from stdin and emits only `dry-run kettle.temperature` for the target
phrase. Frame contents are processed in memory. The relay sends no
Quasar directives. The parser has no BLE integration.

```sh
mkdir -p build/phase1
arm-linux-gnueabihf-as -o build/phase1/quasar-voice-relay.o build/archive/diagnostics/quasar-voice-relay.S
arm-linux-gnueabihf-ld -o build/phase1/quasar-voice-relay build/phase1/quasar-voice-relay.o
```

For a live test, stream the relay over SSH to a temporary executable
on the Station and pipe its stdout into the host parser. Wait for the
relay to start before speaking. Synthetic frames verified that a partial
result is ignored, its final result is accepted once, a repeated final
with the same ID is ignored, and a new request ID is accepted.

The live temperature phrase produced one dry-run event through the
separate Quasar client. The first relay build did not time out and was
stopped explicitly. The timer now uses `setitimer`, and a two-second
Station test confirmed SIGALRM termination. Soloist remained running
with PID 2065. Duplicate handling was verified using synthetic frames.

### Handling the native Alice reply

The user observed the native "kettle is not configured" reply during the
voice dry run. Receiving a final AliceState event is observational.
A live handler needs to suppress the native reply for a matched request
and supply its own spoken response. Soloist sends dialog and TTS
cancellation directives and also pauses playback. For the kettle
adapter, evaluate dialog cancellation independently of playback control.

The first intervention test should use the exact temperature intent and
a fresh final result. Test whether one cancellation, sent immediately
after recognition, beats the native reply on this firmware. Compare with
an ordinary Alice request. Keep the deployed Soloist independent. Enable
intervention only with a working local temperature response or a local
error response for a failed read.

## Standalone Station deployment

The YNDX-0004 exposes two root buses under DWC3/xHCI, USB device nodes
under `/dev/bus/usb`, and `/usr/lib/libusb-1.0.so.0`. The running
kernel has `CONFIG_BT=y`, `CONFIG_BT_LE=y`, and
`CONFIG_MODULES=y`, with `CONFIG_BT_HCIBTUSB` unset. No btusb module
or bluetoothd was found. The DWC2 USB device controller is distinct
from the enumerated host root buses. The snapshot contained no
external USB device. The
[Station Mini specification](https://alice.yandex.ru/support/ru/station/meet/characteristics-mini1)
identifies the USB-C port as the power input. Host enumeration through
the exposed connector while powering the Station remains unverified.

The next hardware gate is to see `0a5c:21ec` in Station-side `lsusb`
using a supported data and power arrangement. If it enumerates, assess
a compatible btusb module plus BlueZ, or a user-space USB HCI transport
through libusb. Both options can target the second controller without
taking the Broadcom UART from BSA. Neither path is deployed.

The built-in-controller route remains separate. Existing BSA opens a
link but has failed during cache processing. Investigate that behavior
offline before another live cache experiment on this Station.

### End-to-end voice prototype

`src/alice-kettle.c` is a one-shot Quasar client for the exact
water-temperature intent. It requires a fresh recognition cycle,
an explicit final result, and deduplicates request IDs in memory.
It sends dialog cancellation and TTS stop for a matched request,
then voices the temperature or a read error via START_VOCALIZE.
A new recognition cycle suppresses a pending stale reply.

`scripts/station-temperature-worker.sh` runs fresh BLE discovery and
the firmware-specific temperature probe. It returns only the decoded
temperature to the voice client. The worker and credential are staged
under `/tmp` for the test. No service or startup file is installed.

The host loopback test sent a retained final, an unrelated utterance,
a partial target, and a repeated target final. It observed exactly
one cancellation pair and a temperature TTS directive. Quasar's
handling of those directives and audible output require a Station
test. The one-shot client exits after a reply or a 90-second wait
for a recognition event.

Build the Station voice client with an ARM hard-float cross compiler:

```sh
arm-linux-gnueabihf-gcc -static -march=armv7-a -mfpu=vfpv3-d16 \
  -std=c11 -O2 -Wall -Wextra -Werror \
  -o build/phase1/alice-kettle src/alice-kettle.c
```

Stage the client, `discovery`, `gatt-temperature`, and the worker
together in a private temporary directory. The BSA probe expects the
key at `/tmp/ble-temp/key`; stage it with mode 0600 and remove it
after testing. Run `alice-kettle` with the absolute worker path.

On the first live run the bridge read 62 °C and cancelled the native
dialog, but the user heard silence. That build closed its Quasar socket
immediately after sending TTS. The follow-up kept the socket open,
read 60 °C, and observed Quasar enter SPEAKING after START_VOCALIZE.
The speaking transition confirms Quasar accepted the directive.
Physical audio and suppression of any competing reply on the
follow-up are awaiting listener confirmation.

#### Live latency baseline

A live request returned 56 °C. Measured from admission of the final
recognition result, cancellation was sent at 1 ms, START_VOCALIZE at
43,050 ms, and Quasar entered SPEAKING at 43,245 ms. The worker measured
7.01 seconds for BLE discovery and 36.01 seconds for its GATT phase.
The successful GATT path has six fixed six-second sleeps: open, cache
preparation, search, and three write completions. Discovery currently
waits a fixed seven seconds. These timings measure from recognized text,
excluding speech recognition itself. Service PIDs stayed unchanged.

The optimized Station build returned 52 °C. Cancellation was sent
within the first millisecond, BLE discovery took 4.01 s, GATT took
0.47 s, and START_VOCALIZE was sent at 4.507 s. Quasar entered
SPEAKING at 4.807 s, down from 43.245 s on the fixed-delay build.
BSA, Quasar and Soloist PIDs stayed unchanged. The test was a one-shot
run and removed the temporary key and executables on exit.

The listener confirmed an audible response in about six seconds.

The temperature worker now tries a direct GATT connection first. Only
a GATT open failure can trigger a single fresh scan and retry. The
worker checks BSA's original PID and listener inode before proceeding
with the fallback. Two live direct reads returned 50 °C and skipped
discovery, with GATT phase times of 0.11 and 0.45 seconds. Host
simulation verified the fallback and its stop conditions.

The repeated direct-first test read 48 °C without a scan and reached
SPEAKING in 641 ms. The listener confirmed the correct audible
temperature and the absence of the native default reply.

## M2 package format decision

Use a YAM v2 `.yampkg` for the first installed build. The Station
already has YAM v2 under `/data/yam`, and its installed Soloist bridge
runs from a YAM package directory. The kettle payload comprises a
Linux ARM32 application, firmware-linked BSA probes, a shell worker,
and default phrases. YAM can package these files together.

The current
[YAM upstream status](https://github.com/nyakokitsu/yam/blob/main/AGENTS.md#implementation-status)
describes service declarations as metadata and does not promise a
built-in supervisor. Verify how the installed Homebrew/YAM integration
starts and stops services before choosing the boot integration. First
prepare a long-lived listener and explicit start/stop/status entry
points. Keep the mutable phrases and negotiated key in package-owned
persistent storage outside the versioned payload. Save the key with
mode 0600 following successful pairing.

The [WASM plugin format](https://github.com/hertebeznat-cell/alice-homebrew/blob/main/docs/PLUGIN_GUIDE.md)
does not provide the local IPC and executable access this payload uses.
The [native plugin SDK](https://github.com/hertebeznat-cell/alice-homebrew/blob/main/docs/NATIVE_SDK.md)
provides a configuration UI, persistent data, and a managed lifecycle,
but requires a single static AArch64 executable. Our probes are ARM32
and use the Station's dynamic libbsa. A native frontend would need a
separate bridge to the packaged service or a wrapper around the ARM32
payload. Consider that frontend after the package runs independently.
