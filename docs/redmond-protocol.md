# Redmond protocol

Status: temperature, status, stop, boil, and target-temperature heating are
implemented. This document keeps the protocol investigation; see
[README](../README.md) for current usage.

## GATT inventory from WSL

USB/IP attached the BCM20702A0 adapter (USB 0a5c:21ec) to WSL.
Linux bound it to btusb as hci0. BlueZ found RFS-KKL003 at the confirmed
address, connected without pairing, and reported ServicesResolved.
Reported RSSI ranged from -89 to -82 dBm during the scan.

The kettle exposed standard services 1800, 1801 and 180a, plus Nordic
UART Service `6e400001-b5a3-f393-e0a9-e50e24dcca9e`. Its characteristics
have UUIDs `6e400002-b5a3-f393-e0a9-e50e24dcca9e` and
`6e400003-b5a3-f393-e0a9-e50e24dcca9e`. The latter has a 2902 client
configuration descriptor. BlueZ disconnected successfully after
enumeration. No Ready for Sky command was sent.

## Ready for Sky status probe

[SkyKettle reference](https://github.com/ClusterM/skykettle-ha/tree/0e8d6ab/custom_components/skykettle)
groups RFS-KKL003 and RK-G214S under the same status schema. It starts
notifications on the UART characteristic ending in 0003, authenticates
with command FF and an eight-byte key, and uses command 06 for status.
The status payload includes current temperature, target temperature,
mode, operating state, and error status.

The WSL probe subscribed to notifications and sent `55 01 06 aa`,
a single status request without authentication. Two runs connected and
completed the GATT write but timed out waiting for a matching status
notification. Two other runs failed to establish a BLE connection.
No application response or temperature was measured. The missing
authentication is a candidate explanation. No new key was enrolled.

With the kettle in pairing mode and after a fresh BlueZ scan, the WSL
probe authenticated with its local eight-byte key and received this
status frame:

```text
55 02 06 00 00 00 00 01 2a 0f 00 00 00 00 00 00 7b 00 00 aa
```

The model-family decoder reported 42 °C, heating off, and error code
0. The target field was 0 and the operating mode field was 0. The
probe sent only authentication and status commands. The initial
attempt in that window needed a fresh scan because the BlueZ device
object was absent. Authentication after reconnect outside pairing mode
was verified in the follow-up below.

### Reconnect with the existing key

Without activating physical pairing mode again, the first reconnect
timed out at GATT service resolution. The next two independent sessions
authenticated with the existing Ready for Sky key and both read 41 °C,
heating off, error code 0. BlueZ reported `Paired: no` and
`Connected: no` after both successful reads. No new key was enrolled
and no heating command was sent.

## WSL ATT baseline

Recorded BlueZ enumeration with btmon on hci0 and copied the trace to
`build/phase1/wsl-kettle-gatt.btsnoop`. Decoded text is in the same
directory with suffix `.decoded.txt`. Both files are mode 0600 and
outside version control. The kettle connected without pairing and
BlueZ resolved services before the local disconnect.

The peripheral answered MTU exchange with 23 and responded to
primary-service and characteristic discovery over ATT. Primary
services were 1800 (0001–0007), 1801 (0008), Nordic UART
(0009–000e), and 180a (000f–ffff, as reported on air). Nordic UART
notification value handle is 000b (declaration 000a) and the write
value handle is 000e (declaration 000d). Manufacturer name value is
0011 (declaration 0010). The earlier BlueZ inventory showed CCCD
000c. This trace did not contain a separate on-air lookup for that
descriptor. Attribute Not Found terminates several successful
discovery ranges. No Ready for Sky authentication or status write
was sent in this capture.

## Proposed kettle module operations

| Module operation | Ready for Sky operation | Verification |
| --- | --- | --- |
| Read current temperature, operating state, mode, target, and error | Authenticate (FF), then get status (06) | Status read verified on this kettle through the Station. |
| Read firmware version | Get version (01) | Reference implementation only. Diagnostics use. |
| Stop heating | Turn off (04), then read status (06) | Verified on this kettle. |
| Start boiling | Set boil mode (05), turn on (03), then read status (06) | Verified on this kettle. |
| Heat to a selected temperature | Set mode and target (05), turn on (03), then read status (06) | Implemented; the 35–90 °C range is untested on this kettle. |

Use the module operations as the public interface. Keep authentication,
sequence numbers, packet framing and notification validation inside
the protocol layer. A timed-out start must be reconciled through
a status read before considering another start. The first release
does not require schedules, lighting, sound, or usage statistics.

[SkyKettle command definitions](https://github.com/ClusterM/skykettle-ha/blob/0e8d6ab/custom_components/skykettle/skykettle.py#L72)

## Stop-heating voice prototype

The `GATT_STOP=1` variant of the temperature probe authenticates and
reads status before acting. Heating modes are boil, heat, and boil
plus heat. If the kettle is idle or in a non-heating mode, the probe
reports idle and sends no stop request. If heating is active, it sends
the stop opcode 04, checks its acknowledgement, and issues a new
status request with a distinct sequence. It confirms stop only when
that status shows the kettle off. No path starts heating.

Build the variant with the same flags as the temperature probe plus
`--defsym=GATT_STOP=1`. Stage it as `gatt-stop` beside
`gatt-temperature` for the Station worker. The voice client dispatches
the exact “Останови кипячение” phrase to that variant.

Both branches ran on the Station. The idle branch generated the
no-heating response. During active heating, the stop acknowledgement
and a separate status response confirmed heating off. Quasar entered
SPEAKING in both tests, at 472 ms and 616 ms from final recognition.

The listener heard no reply on the original “Останови кипячение”
utterance despite a brief SPEAKING state. A 900 ms delay did not fix
that phrase. The alternate recognized phrase “Прекрати нагрев
чайника” reached the same stop handler and audibly returned the
idle message. Use this phrase for the currently verified audible
idle flow. The reason for the original phrase's interruption is
unverified.

## Voice status query

“Какой статус чайника?” performs one authenticated status read (06)
and speaks the heating state and approximate water temperature. During
heating it also reports the status target (frame byte 5). Pure boiling
uses the nominal 100 °C target because its protocol target field is zero.
The heating state is derived from the running flag and selected
heating mode, using the same check as stop. This field describes an
active heating program and cannot distinguish whether the element is
energized at an instant during temperature maintenance. The worker
uses the existing temperature probe and sends no control opcode.

## Voice boil command

“Вскипяти воду в чайнике” selects the `boil` operation. The worker uses
a separate Station probe built with all temperature flags plus
`--defsym=GATT_BOIL=1` and staged as `gatt-boil`. The other variants
do not contain the boil templates.

The probe first reads authenticated status. If a program is already
active, the voice response is “Чайник уже работает”. From idle with
no reported error, the probe sets mode 0 with opcode 05, preserving the
reported boil-time adjustment. It reads status again to verify the
mode and idle state before issuing opcode 03 once. A fresh status
read then must show boil mode, running, and error code zero before
the voice response confirms the start. The probe also checks status
after an uncertain start write or acknowledgement.

ARM32 builds and a simulated Quasar voice dispatch test passed.
No live kettle start was attempted. A supervised check on the physical
kettle and separate approval are still required.

The supervised Station test confirmed both voice operations. The
listener observed heating start after “Вскипяти воду в чайнике”.
The stop handler confirmed heating off after “Прекрати нагрев чайника”.
A separate final status read reported heating off and 54 °C. Quasar
entered SPEAKING at 849 ms for start and 1023 ms for stop. BSA,
Quasar and Soloist PIDs stayed stable.

## Target-temperature heating

The existing boil probe accepts an optional two-digit target from 35 to
90. With a target, it uses mode `01` and writes the exact target in the
existing model-family `05` frame. It requires an idle, error-free initial
status and skips the start if the water is already at or above the target.
It checks the target, mode and idle state before the single `03` start
request and confirms the target, mode and running state via a fresh status
afterward. Without a target it retains the boil flow. The command range
follows the model-family reference and has not been tested on this kettle.
