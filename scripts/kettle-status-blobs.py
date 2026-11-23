#!/usr/bin/env python3
"""Build the BSA write and notification bodies for a Ready for Sky status read.

The Station server reads a 244-byte write at these offsets:
  0    connection id, filled by the probe
  4    characteristic id, 52 bytes
  56   write type
  58   value length
  60   value
  240  descriptor flag

A characteristic id is a 28-byte service id followed by a 24-byte UUID.
UUID length is at byte 0 and the UUID bytes start at byte 4. Notification
registration is 64 bytes: client interface at 0, characteristic id at 4,
and the peer address at 56.
"""

import struct
from pathlib import Path

WRITE_SIZE = 244
NOTIFY_SIZE = 64
NUS = bytes.fromhex("6e400001b5a3f393e0a9e50e24dcca9e")[::-1]


def nus(last):
    raw = bytearray(NUS)
    raw[12] = last
    return bytes(raw)


def put_uuid(buf, offset, payload, length):
    struct.pack_into("<H", buf, offset, length)
    buf[offset + 4:offset + 4 + len(payload)] = payload


def put_characteristic(buf, offset, service, characteristic):
    put_uuid(buf, offset, service, 16)
    buf[offset + 24] = 1
    put_uuid(buf, offset + 28, characteristic, 16)


def write_body(service, characteristic, value, descriptor=None):
    body = bytearray(WRITE_SIZE)
    put_characteristic(body, 4, service, characteristic)
    body[56] = 2
    struct.pack_into("<H", body, 58, len(value))
    body[60:60 + len(value)] = value
    if descriptor is not None:
        put_characteristic(body, 164, service, characteristic)
        put_uuid(body, 216, descriptor, len(descriptor))
        body[240] = 1
    return bytes(body)


def notify_body(service, characteristic):
    body = bytearray(NOTIFY_SIZE)
    put_characteristic(body, 4, service, characteristic)
    return bytes(body)


def bodies():
    service = nus(1)
    notify = nus(3)
    write = nus(2)
    cccd = write_body(service, notify, b"\x01\x00", struct.pack("<H", 0x2902))
    auth = write_body(service, write, b"\x55\x01\xff" + bytes(8) + b"\xaa")
    status = write_body(service, write, b"\x55\x02\x06\xaa")
    registration = notify_body(service, notify)
    return cccd, auth, status, registration


def check(cccd, auth, status, registration):
    if list(map(len, (cccd, auth, status, registration))) != [244, 244, 244, 64]:
        raise AssertionError("body size")
    if cccd[8:24] != nus(1) or cccd[36:52] != nus(3):
        raise AssertionError("cccd characteristic")
    if cccd[56] != 2 or struct.unpack_from("<H", cccd, 58) != (2,) or cccd[60:62] != b"\x01\x00":
        raise AssertionError("cccd value")
    if cccd[216:218] != b"\x02\x00" or cccd[220:222] != b"\x02\x29" or cccd[240] != 1:
        raise AssertionError("cccd descriptor")
    if auth[36:52] != nus(2) or auth[240] != 0:
        raise AssertionError("auth characteristic")
    if struct.unpack_from("<H", auth, 58) != (12,) or auth[60:63] != b"\x55\x01\xff" or auth[71] != 0xAA:
        raise AssertionError("auth frame")
    if auth[63:71] != bytes(8):
        raise AssertionError("auth key must stay outside the template")
    if status[60:64] != b"\x55\x02\x06\xaa" or struct.unpack_from("<H", status, 58) != (4,):
        raise AssertionError("status frame")
    if registration[8:24] != nus(1) or registration[36:52] != nus(3):
        raise AssertionError("notification characteristic")


def emit(path):
    cccd, auth, status, registration = bodies()
    check(cccd, auth, status, registration)
    def block(name, blob):
        rows = []
        for offset in range(0, len(blob), 16):
            row = ", ".join("0x%02x" % byte for byte in blob[offset:offset + 16])
            rows.append("    .byte " + row)
        return name + ":\n" + "\n".join(rows)
    text = "\n".join((
        block("cccd_template", cccd),
        block("auth_template", auth),
        block("status_template", status),
        block("notif_template", registration),
        '.ifdef GATT_STOP',
        block("stop_template", write_body(nus(1), nus(2), b"\x55\x03\x04\xaa")),
        block("stop_status_template", write_body(nus(1), nus(2), b"\x55\x04\x06\xaa")),
        '.endif',
        '.ifdef GATT_BOIL',
        block("boil_mode_template", write_body(
            nus(1), nus(2), b"\x55\x03\x05" + struct.pack("<BxBxxxxxxxxxxBxx", 0, 0, 0x80) + b"\xaa",
        )),
        block("boil_check_template", write_body(nus(1), nus(2), b"\x55\x04\x06\xaa")),
        block("boil_start_template", write_body(nus(1), nus(2), b"\x55\x05\x03\xaa")),
        block("boil_status_template", write_body(nus(1), nus(2), b"\x55\x06\x06\xaa")),
        '.endif',
        "",
    ))
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    print("wrote", path)


if __name__ == "__main__":
    emit("build/phase1/kettle-status-blobs.inc")
