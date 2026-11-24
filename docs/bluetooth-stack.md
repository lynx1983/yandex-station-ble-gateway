# Bluetooth stack investigation

Status: the implemented client path is summarized in
[Gateway architecture](architecture.md); runtime failures are covered in
[Troubleshooting](troubleshooting.md). This document preserves the evidence
used to select the transport.

The initial Station investigation recorded the following BSA launch command on the YNDX-0004:

```text
/system/workdir/bin/bsa_server -d /dev/ttyS1 -p /etc/bluetooth/BCM43430.hcd -r13 -u /tmp/
```

The socket path, client library, ELF ABI, and SDK availability still need verification. The `-u /tmp/` argument is a lead for locating Unix endpoints. A socket listing alone cannot establish that a second client can register or safely scan alongside Quasar.

## Collect a read-only snapshot

From the repository root in WSL, substitute the existing root SSH target or alias for `root@STATION_IP`:

```sh
ssh -T root@STATION_IP 'sh -s' < scripts/bsa-diagnostics.sh > /tmp/yndx-0004-bsa.txt 2>&1
```

The script runs through SSH standard input and writes the report to WSL's `/tmp`. It reads process metadata, kernel socket tables, file names, and selected startup lines. It issues no BSA commands and opens no Bluetooth session. Review command lines and network addresses before sharing the report.

## Interpret the evidence

1. Match `socket:[inode]` descriptors with the inode column in `/proc/net/unix`, `tcp`, and `tcp6` to identify endpoints held by BSA and candidate clients. Unnamed or transient sockets may require further investigation.
2. Compare mapped libraries and executable paths with the file inventory. Search results are candidates, and an empty result does not exclude a statically linked or differently named client.
3. Use host-side `file` and `readelf -h -l -d --dyn-syms` on copies of selected artifacts to inspect architecture, interpreter, dependencies, and exported functions. Determine the ABI from the actual artifacts before selecting a compiler.
4. Identify client registration and BLE discovery entry points from the resulting evidence. Assess coexistence with Quasar and Bluetooth audio before proposing an on-device scan.

## Station snapshot

The read-only SSH collector ran as root via the `station` alias on 2026-11-20. WSL holds the report at `/tmp/yndx-0004-bsa.txt` and the library copy at `build/work/libbsa.so`.

| Observation | Evidence |
| --- | --- |
| BSA controls the UART | `bsa_server` PID 3277 held `/dev/ttyS1` on FD 5. |
| BSA owns a Unix stream listener | FD 9 matched inode 16304 at `/tmp/./bt-daemon-socket`. FD 10 matched connected inode 19217 at that path. |
| The firmware ships a BSA library | `/system/workdir/lib/libbsa.so` is a stripped ARM32 EABI5 hard-float shared object with a `libc.so.6` dependency and the embedded string `./bt-daemon-socket`. |
| The library exposes candidate client entry points | Dynamic exports include `BSA_MgtOpen`, `BSA_MgtClose`, `BSA_DiscStart`, `BSA_BleClAppRegister`, and corresponding initialization functions. |
| A Quasar-side startup script exists | The inventory found `/system/vendor/quasar/bsa_server_run.sh`. Its contents were outside this snapshot. |

The snapshot did not identify the connected peer or find matching SDK headers. The exported symbols identify a candidate API. The discovery parameters, controller behavior, and ability to open an additional client session still require verification. No BSA connection or BLE scan was attempted.

## Second-client transport check

Host inspection of the shipped ARM hard-float `bsa_server` found an `accept` import and diagnostic strings for accepted clients and a maximum client count. These are clues to client handling. The active limit and disconnect behavior are unverified.

A payload-free probe attempted SSH Unix socket forwarding to `/tmp/bt-daemon-socket`. The Station's SSH server denied the forwarding channel before it reached BSA. BSA PID 3277, all four observed `maind` PIDs, and the existing BSA socket inodes were the same before and after. BusyBox has no `nc` applet on this image. A Station-side probe is required for a live result. Process and socket checks only measure continuity of those resources. Alice and Bluetooth playback require functional checks.

### Direct transport probe

A statically linked ARM32 probe was streamed to a temporary directory on the Station. It opened `/tmp/bt-daemon-socket` for three seconds without sending bytes. The SSH shell removed the executable and directory on exit.

| Snapshot | BSA listener | Original connection | Probe connection |
| --- | --- | --- | --- |
| Before | FD 9, inode 16304 | FD 10, inode 19217 | Absent |
| During | FD 9, inode 16304 | FD 10, inode 19217 | FD 11, inode 51654 |
| After | FD 9, inode 16304 | FD 10, inode 19217 | Absent |

Both connections were present on the server simultaneously. BSA PID 3277, its UART descriptor, and all four observed Quasar `maind` PIDs stayed unchanged. The result verifies Unix transport coexistence. BSA API registration, BLE discovery, and Alice functionality were outside this probe.

### Management session probe

The ARM32 probe used the Station's `libbsa.so` (GNU build ID
`fa288387276e4346d28c0c935b8202ae96965f87`). Host disassembly established
that `BSA_MgtOpenInit` zeros 104 bytes, `BSA_MgtOpen` requires a callback
pointer at offset 100, and `BSA_MgtCloseInit` zeros four bytes. The probe
supplies a no-op callback and does not interpret event data. These layouts
are specific to the inspected library.

Build from the repository root with the matching library copy in
`build/work/libbsa.so`:

```sh
arm-linux-gnueabihf-as -o build/work/bsa-management-probe.o scripts/bsa-management-probe.S
arm-linux-gnueabihf-ld --dynamic-linker /lib/ld-linux-armhf.so.3 \
  --allow-shlib-undefined -Lbuild/work -o build/work/bsa-management-probe \
  build/work/bsa-management-probe.o -lbsa
```

The probe ran from the Station's `/tmp` with
`LD_LIBRARY_PATH=/system/workdir/lib` and `LD_PRELOAD=libpthread.so.0`.
The SSH shell removed the temporary executable and log after the test.

`BSA_MgtOpen` and `BSA_MgtClose` both returned 0. The probe exited with
status 0. Connected inode 52052 appeared during the three-second hold and
disappeared after close. The original connected inode 19217, listener
16304, BSA PID 3277, and all four observed `maind` PIDs stayed present
in all three snapshots. This verifies management API session coexistence
under the observed conditions. BLE discovery, GATT registration and Alice
response were outside the test.

### Bounded BLE discovery

The firmware-specific probe has an optional discovery mode. Assemble it
with `--defsym=DISCOVERY=1` and use the same linker and Station runtime
settings as for the management probe.

The shipped library initializes a 32-byte discovery parameter block,
requires the callback at offset 4, and defaults the mode byte at offset
24 to 1. The probe sets mode to 0x10 and duration to 4 inquiry units
(about 5.12 seconds). Event 0 means a device report and event 1 means
completion, confirmed in the firmware callback dispatcher. The probe
logs those event types without decoding the payload. It waits seven
seconds for completion and closes the management session. The process
has a 12-second watchdog. The test uses the configured inquiry duration
because the scope of the server-side abort operation is unverified.

References used to interpret the initialization and BLE inquiry mode:

- [Firefly Linux Broadcom BSA example and header](https://gitlab.com/firefly-linux/external/broadcom_bsa/-/tree/d7855e9dc601d69b00aa5e52ab03e68f35ac5aa5/3rdparty/embedded/bsa_examples/linux)
- [AOSP Bluetooth inquiry definitions](https://android.googlesource.com/platform/system/bt/+/8986635/stack/include/btm_api.h)

On the Station, `BSA_DiscStart` returned 0. The probe received multiple
`BSA_DISC_NEW_EVT` reports followed by `BSA_DISC_CMPL_EVT`.
`BSA_MgtClose` returned 0 and the probe exited with status 0. Probe
socket inode 52542 disappeared after exit. BSA PID 3277, all four observed
Quasar PIDs, listener inode 16304, and original connected inode 19217
were present throughout. No Bluetooth connection to a discovered device
was initiated. The probe did not identify the kettle or test Alice.

### Device inventory

The discovery probe now captures address and name bytes from each device
event. Probe diagnostics go to stderr. Redirect stdout to a local binary
file and decode it with
`python3 scripts/decode-bsa-discovery.py build/archive/station-investigation/discovered-devices.bin`.
The decoder groups reports by address. The probe copies up to 100 name
bytes and stops reading at the first NUL.

The first successful inventory contained 11 unique addresses, seven
unnamed. One reported the name `RFS-KKL003`. The
[SkyKettle integration](https://github.com/ClusterM/skykettle-ha) lists
that name and RK-G214S as supported entries. This is a kettle candidate
whose physical identity is still unverified. A scan with the target
kettle off its powered base, followed by a scan after returning it,
can correlate that candidate with the physical device.

With the kettle off its base, the same scan found 14 unique addresses.
The previous `RFS-KKL003` address `AA:BB:CC:DD:EE:FF` did not appear.
Other devices were detected and the scan completed. After
restoring the kettle to the powered base, the same scan found 15 unique
addresses, including the original RFS-KKL003 name and address. This
correlates the advertisement with the physical kettle at the current
Station location.

### GATT registration

The probe's `--defsym=GATT_REGISTER=1` build opened its BSA management
session and tried to register a GATT client using the shipped library.
`BSA_BleClAppRegister` returned 1601 (`BSA_ERROR_SRV_BLE_NOT_ENABLED`
in the Firefly SDK status header). Management close returned 0.
BSA and Quasar PIDs remained stable in the first attempt. Inquiry
therefore works with the current stack state, while GATT client
registration requires an additional step. The Firefly example calls
`BSA_BleEnable` before registering. Enabling is a server state change
and has not been attempted. No connection to the kettle was opened.

With explicit approval, a build using `--defsym=GATT_REGISTER=1
--defsym=GATT_ENABLE=1` called `BSA_BleEnable` before registration.
Enable, register, deregister, and management close each returned 0.
BSA, Quasar and Soloist PIDs and the original BSA socket inode were
unchanged across snapshots. No GATT connection or profile disable call
was issued. A fresh management session without an enable call again
received status 1601, so subsequent probes must enable BLE in the same
session before registering. The test did not distinguish per-session
state from automatic cleanup when the enabling client exits.

### GATT connection and service search incident

The ARM32 probe built with `GATT_REGISTER`, `GATT_ENABLE` and
`GATT_SEARCH` targeted the confirmed kettle address
`AA:BB:CC:DD:EE:FF`. Both attempts observed a successful
`BSA_BLE_CL_OPEN_EVT` (status 0, connection ID 4). The first attempt
timed out waiting for service discovery before closing the link. The
capture contains a late completion with status 129 and no service results.
The original BSA PID and the observed Quasar and Soloist PIDs remained.

The second attempt responded to the BSA cache-load event with an empty
local cache, then requested service enumeration. The client trace records
both calls followed by loss of the BSA socket. The client exited with
105. A separate read-only check confirmed no BSA PID and no listener at
the original socket path. Quasar and Soloist retained their PIDs. The
kernel log check produced no matching BSA or segfault lines. The sequence
establishes when the server became unavailable. The cause is unverified.

Suspend on-device GATT probes pending BSA recovery and diagnosis.
Restart or recovery actions require user authorization. Neither test
issued a GATT characteristic write or a kettle heating command. Raw
captures are under the ignored `build/archive/station-investigation/` directory.

### BSA recovery

With user approval, ran `/system/vendor/quasar/bsa_server_run.sh 1`
because the server process was absent. The script toggled rfkill and
started BSA. Read-only follow-up found PID 6301 holding `/dev/ttyS1` and
listening at `/tmp/bt-daemon-socket` (inode 65440). The rfkill state
was 1. Existing Quasar and Soloist PIDs remained. The follow-up showed
only the listener at the BSA socket path, so reconnection by the
original client and functional Bluetooth recovery remain unverified.
No further GATT experiments were run.

### Connection retries at the original location

After recovery, ran three connection-only attempts against the same
kettle address at the unchanged Station location. The build adds
`--defsym=GATT_CONNECT_ONLY=1` to the GATT search build flags and imports
no search, cache-load, read, or write functions.

Each attempt opened a separate BSA session, enabled BLE, registered the
GATT client, and received an open event with status 133 (0x85) and
connection ID 0. Client deregistration and management close succeeded
each time. The first run stopped on the unexpected status. The remaining
two attempts ran after confirming the callback status and the original
listener. BSA PID 6301, listener inode 65440, and Quasar and Soloist
PIDs persisted. The test did not measure RSSI or verify whether a phone
was connected. These results do not establish the cause of the failure.

### Connection retries closer to the kettle

After the user moved the Station closer, the same connection-only probe
again received status 133 in all three attempts. Deregistration and
management close succeeded in every attempt. BSA PID 2309, listener
inode 3631, and the observed Quasar and Soloist PIDs stayed stable
during the series. The BSA socket table showed an additional connected
server-side socket at the end. Its peer was not identified.

A separate discovery pass from the new position found 11 distinct
addresses and confirmed that the kettle still reports as RFS-KKL003
at AA:BB:CC:DD:EE:FF. The two retry series have the same result.
There is no RSSI or phone connection state measurement, and the BSA
process baseline changed between the series.

### Fresh scan with phone Bluetooth off

At the near position, after the user disabled Bluetooth on the phone,
a fresh scan found RFS-KKL003. The connection-only probe ran immediately
afterward in a new BSA session. It received open status 0 with connection
ID 5, a cache-load event, and close status 0. The probe did not answer
the cache request because this was a connection-only test.
Deregistration and management close also returned 0. BSA, Quasar and
Soloist PIDs, the listener, and the existing accepted BSA socket were
still present after the test.

This test changed both phone Bluetooth state and discovery timing.
It establishes a successful connection under those conditions.

### Service search without a cache reply

With the phone Bluetooth off and the Station close to the kettle, a
fresh scan found RFS-KKL003. The probe built with
`--defsym=GATT_SKIP_CACHE_REPLY=1` connected successfully (open status
0, connection ID 4). BSA requested a cache load for connection 4.
The probe sent the service search request without answering the cache
event and reached its six-second search timeout. There were no service
records. The capture contains a late search completion with status
129 (0x81). Deregistration and management close returned 0.
BSA PID 2309, listener inode 3631, accepted inode 12126, and the
observed Quasar and Soloist PIDs stayed present. No attribute writes
were issued. This isolates a reproducible stall and identifies the
cache handshake as the next item to verify offline against the
Station server before sending another cache reply.

### Offline cache handshake review

The Station library build ID is
`fa288387276e4346d28c0c935b8202ae96965f87` and the server build ID is
`055a0ea7aeb4b7f9acd6cc909d0f1268f1c3c003`. The library's cache-load
API initializes and sends 296 bytes. In the server, the request
handler also allocates and copies 296 bytes before scheduling the
cache-load callback. The callback validates the connection ID at
offset 6 and the peer address at offset 8. It reads the continuation
flag at offset 0, the attribute count at offset 2, and uses attribute
data at offset 16. These fields agree with the failed probe's empty
reply and the Firefly SDK example. The server queues an internal GATT
event even when the attribute count is zero. The immediate API reply
does not establish that the internal event was processed.

This review found no size or field-offset mismatch for the empty reply.
It did not establish why BSA exited after the cache response and service
search were sent in the same attempt. The search build now omits the
cache reply by default. The experimental cache path requires
`--defsym=GATT_REPLY_CACHE=1`. Both variants were built on WSL and
their dynamic imports checked. Neither was deployed during this review.

The live diagnostic that separates the two operations is a cache-only
test with BSA health checks and no service search. Because even that
test may stop the active server, run it in an approved maintenance
window with an agreed recovery action, or use an isolated target.

### Cache-only incident

The approved cache-only probe used `GATT_REPLY_CACHE=1` and
`GATT_CACHE_ONLY=1`. Its dynamic imports contained the cache API and
no service-search API. A fresh scan found the kettle. GATT opened with
status 0 and connection ID 4, followed by a cache request for that
connection. `BSA_BleClCacheLoad` returned 0. During the subsequent
wait and cleanup the client lost BSA and exited with 105. The BSA
process and socket were absent after the run and in a separate
read-only check. Quasar and Soloist PIDs remained. No kernel log lines
matched the server name or a segfault. This confirms that the failure
can occur without a service search request. The precise failure inside
BSA is unverified. Further cache tests are suspended.

Following the cache-only incident, the user approved recovery through
the stock launch script. BSA returned as PID 2890, held `/dev/ttyS1`,
and listened on Unix socket inode 16133 with rfkill state 1.
Quasar and Soloist retained their PIDs. The follow-up snapshot
showed only the listener, so the native client's reconnection and
functional Bluetooth recovery were not verified.

### Direct attribute read review

The Station library exports `BSA_BleClRead`, but no raw
attribute-handle read in the BSA GATT client API. The station server's
read handler accepts a 136-byte request, takes the connection ID at
offset 0, and passes the characteristic identifier at offset 4 (or the
descriptor identifier at offset 56) to the internal GATT API. These
inputs are nested UUID and instance identifiers. The handles learned
through BlueZ cannot be supplied in their place.

The [related Bluedroid read implementation](https://android.googlesource.com/platform/external/bluetooth/bluedroid/+/ffa5c93d455f33af1be3b04d39520413dc24afd5/bta/gatt/bta_gattc_act.c)
resolves those identifiers through the discovered attribute database.
Its [discovery state machine](https://android.googlesource.com/platform/external/bluetooth/bluedroid/+/master/bta/gatt/bta_gattc_main.c)
queues reads and writes while discovery is pending. This is comparative
evidence, not source for the exact Station firmware. Combined with
the Station's observed cache request and stalled search, it provides
no verified bypass through the exported API. No live read, cache
reply, or key transfer was attempted in this review.

### BTA/GATT diagnostic options

Read-only inspection found that the running server sends both standard
output and standard error to `/dev/null` and has a zero soft core dump
limit. No candidate crash dump, btsnoop capture, or GATT log was found
in the checked directories. The shipped binary advertises
`-gatt=#` and `-b file`. The former enables GATT-layer tracing;
the latter records HCI serial traffic. The library also exports
`BSA_TmSetTraceLevel`, but changing verbosity would still leave the
current output descriptors pointed at `/dev/null`.

A WSL BlueZ `btmon` trace can establish the peripheral's expected
ATT discovery sequence and attribute handles independently. It cannot
show where Station BSA exits. For a Station-side trace, use a separate,
approved maintenance window to start the same server binary and
firmware with GATT diagnostics directed to `/tmp`, optionally capture
HCI traffic, and compare the last BTA event with the last ATT packet.
Only after capture and recovery are ready, consider one cache-only
reproduction. It has already stopped the active server, so it is
unsuitable as a routine retry. No trace settings were changed here.

### Instrumented Station discovery

With explicit approval, the stock server binary was temporarily started
with the same UART, firmware and socket path plus GATT tracing and HCI
snoop output in tmpfs. After fresh discovery, the cache-only probe
connected and acknowledged the empty cache reply. The probe imported no
explicit service-search call. The reply itself was followed by internal
ATT discovery. The capture shows primary service results for 1800,
1801, Nordic UART, and 180a, the Nordic UART value handles 000b and 000e,
and the CCCD at 000c. These match the WSL baseline.

The last recorded ATT exchange was Find Information for 0012–ffff,
followed by Attribute Not Found (0x0a). The WSL baseline contains the
same terminating response and completes successfully. The Station
server then disappeared before it returned a service list to the
client. The GATT text log ends during descriptor discovery and is
buffered, so its last printed line alone does not identify the
instruction that stopped the process.

Offline comparison of the capture, the client trace, and the matching
Android 5.1.1 GATT-cache implementation narrows the failure:

- The probe event log contains only GATT open and the initial cache-load
  request. The next client-side line is `Socket disconnected from Server`
  at 01:00:37.510. The last flushed server HCI line is the Find
  Information transmit at 01:00:37.491. The server died in the few
  milliseconds after the Attribute Not Found reply, before any
  cache-open, cache-save, or service-list message reached the client.
- `gatt_proc_disc_error_rsp` turns reason 0x0a into a successful end of
  the current operation. The reply itself is the normal end of the last
  descriptor range, not an error that the stack treats as fatal.
- The log line `free byte = 26, req 48` is the ordinary “allocate the
  next 128-byte GATT DB buffer” check. GAP’s three 16-bit attributes
  account for 128 − 48 − 3×18 = 26. The following `free byte = 80` is
  128 − 48. This is not a buffer overrun at service insertion.
- `bta_gattc_co_cache_save` copies into a fixed 296-byte message and
  would smash saved registers for more than 10 attributes. Its only
  caller, `bta_gattc_cache_save`, stops the batch at 10, which fills
  that message exactly. That copy never started: the client received no
  save event, and the save runs only after the client answers the
  cache-open that discovery completion sends.
- After the last characteristic’s descriptors are done, the server sets
  the server-cache state to save and calls `bta_gattc_co_cache_open`
  from the discovery thread, before waiting for the client. That call
  is the first point at which a completion message would leave the
  process. It did not. The crash is inside the synchronous completion
  that follows the last Find Information, which includes the cache
  display and that cache-open.

The Device Information service is the only one stored with end handle
0xffff, so its manufacturer characteristic is the one whose descriptor
search covers 0012–ffff. That search is the last ATT exchange before
the process disappears. The faulting instruction inside the completion
is not yet identified. Cache and GATT experiments on the Station remain
suspended.

Automatic cleanup restored the stock BSA launch (PID 3819, listener
inode 39078). Quasar and Soloist PIDs remained stable. The local
captures are in `build/archive/station-investigation/station-capture/`. Original temporary
capture files remain in Station tmpfs. No Ready for Sky commands or
application credential were sent.

### Prepared cache-load reply

The incoming handler at `0x10cccc` reads `more` at offset 0 and
`num_attr` at offset 2. It passes the attribute array at offset 16 to
the internal load. `more == 0` becomes status 0. `more != 0` becomes
status `0x8a`, which asks the client for another batch. Status 0 or
`0x8a` with a non-zero count calls `bta_gattc_rebuild_cache` at
`0xcb354` and does not start on-air discovery. A zero count is the path
that discovered the kettle and then exited.

`bta_gattc_rebuild_cache` strides 28 bytes. A 16-bit UUID length is at
byte 0 and the value is at byte 4. Service type is 3, characteristic
type is 1, descriptor type is 2. Handles are at bytes 20 and 22.
`scripts/kettle-cache-load.py` builds one 296-byte message for
connection ID and address supplied by the caller. It carries eight
attributes: primary 1800 (`0001–0007`), 1801 (`0008`), Nordic UART
(`0009–000e`) with notify value `000b`, CCCD `000c` and write value
`000e`, and 180a (`000f–0011`) with manufacturer value `0011`. The
180a end handle is the last attribute confirmed on air, not `0xffff`.
The host check confirms the size, the header fields, the stride, and
the little-endian 128-bit UUIDs.

### Cache-image run

With approval, a fresh BLE scan found RFS-KKL003 at AA:BB:CC:DD:EE:FF.
The cache-image probe then connected, received the cache-load request
for connection 5, and sent the eight-attribute message. `BSA_BleClCacheLoad`
returned 0. During the following wait the client logged no search-result
and no search-complete event. The probe then closed connection 5 with
status 0, deregistered, and closed the management session.

BSA remained PID 3819 and the Unix listener kept inode 39078. Quasar
and Soloist PIDs stayed 2376, 2273, 2195, 2194, and 2107. No Ready for
Sky command was sent. The server no longer exits on this reply, and the
client still does not receive the rebuilt database: this probe does not
call `BSA_BleClSearch`. Captures are in `build/archive/station-investigation/station-cache-image/`.

### Cache image followed by service search

The follow-up probe sent the same eight-attribute load, waited, then
called `BSA_BleClSearch` with an empty UUID so the server would return
every cached service. It did not import a read or write call. After a
fresh scan found the kettle, the search completed with status 0 on
connection 4. The client received four service results: 1800, 1801,
Nordic UART `6e400001-b5a3-f393-e0a9-e50e24dcca9e`, and 180a. That is
the set loaded in the cache message. BSA stayed PID 3819 with listener
inode 39078, and the Quasar and Soloist PIDs stayed unchanged. This
run did not capture HCI, so it does not by itself show whether any ATT
request left the controller. The previously fatal empty-cache discovery
did not recur. Captures are in `build/archive/station-investigation/station-cache-search/`.

### Read-only kettle temperature

The temperature probe uses the same cache image and service search, then
registers for Nordic UART notifications, writes CCCD `000c` with
`01 00`, authenticates with opcode `0xFF`, and sends the read-only
status opcode `0x06`. It does not send a heating command. The eight-byte
key is read from `/tmp/ble-temp/key` at runtime and is not linked into
the probe.

A characteristic or descriptor whose cache end handle is 0 makes the
server reject a later write with GATT status `0x85`. The notify value,
CCCD, write value, and manufacturer value now use an end handle equal
to their start handle. With that cache, one session completed
authentication and status. The status notification was
`55 02 06 00 00 00 00 01 52 0f … aa`: 82 °C, heating off, error 0.
`BSA_BleClClose` returned 0. BSA stayed PID 3819 with listener inode
39078, and the Quasar and Soloist PIDs stayed unchanged. Captures are
in `build/archive/station-investigation/station-temperature/`.

An earlier session with the same corrected cache returned 86 °C in the
status frame and then crashed while formatting the decimal: the text
template lives in read-only data. The formatter now writes a BSS copy.
A session between those two returned `0x85` on the authentication write
after the CCCD write had already succeeded, then the link was already
gone when the probe closed it. The following session is the clean
82 °C result above.

#### Rebuild the temperature probe

From the repository root, place a copy of the firmware-matched
`libbsa.so` in `build/work/`, then run:

```sh
python3 scripts/kettle-status-blobs.py
arm-linux-gnueabihf-as \
  --defsym=GATT_REGISTER=1 --defsym=GATT_ENABLE=1 \
  --defsym=GATT_SEARCH=1 --defsym=GATT_REPLY_CACHE=1 \
  --defsym=GATT_CACHE_IMAGE=1 --defsym=GATT_CACHE_ONLY=1 \
  --defsym=GATT_CACHE_THEN_SEARCH=1 --defsym=GATT_TEMPERATURE=1 \
  -o build/work/gatt-temperature.o scripts/bsa-management-probe.S
arm-linux-gnueabihf-ld --dynamic-linker /lib/ld-linux-armhf.so.3 \
  --allow-shlib-undefined -Lbuild/work -o build/work/gatt-temperature \
  build/work/gatt-temperature.o -lbsa
```

Use the full flag set for this firmware. In particular,
`GATT_CACHE_IMAGE` selects the nonempty cache. For a device run, stage
the probe and the eight-byte key in the root-only `/tmp/ble-temp`
directory, run a fresh BLE scan first, then run the probe from `/tmp`
with `LD_PRELOAD=libpthread.so.0` and
`LD_LIBRARY_PATH=/system/workdir/lib`. Remove the staged key after use.
The probe is a one-shot CLI. The package runs it from a long-lived
service, retries once after a GATT open failure, and voices the result
through `alice-kettle`; see [README](../README.md).
