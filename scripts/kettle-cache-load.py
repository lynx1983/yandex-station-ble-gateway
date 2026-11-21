#!/usr/bin/env python3
"""Build the 296-byte BSA_BleClCacheLoad body for the RK-G214S.

The Station server copies this body unchanged and, when the continuation
field is zero, posts an internal cache-load with status 0 and the
attribute pointer at offset 16. A non-zero attribute count then rebuilds
the GATT database instead of starting on-air discovery.

Layout checked against bsa_server build 055a0ea7aeb4b7f9acd6cc909d0f1268f1c3c003:
  0   UINT16 more          0 finishes the load; non-zero becomes status 0x8a
  2   UINT16 num_attr      at most 10
  4   UINT16 attr_index    not read by the incoming handler
  6   UINT16 conn_id
  8   UINT8  bd_addr[6]
  14  padding to 4-byte alignment
  16  tBTA_GATTC_NV_ATTR[10], 28 bytes each

Each attribute is the SDK tBTA_GATTC_NV_ATTR. The server reads it at
bta_gattc_rebuild_cache (0xcb354) and bta_gattc_add_attr_to_cache (0xc9990):
  0   tBT_UUID, 20 bytes; len at 0, value at 4
  20  UINT16 s_handle      characteristic value handle, or service start
  22  UINT16 e_handle      inclusive end; a zero end is rejected (GATT_ERROR 0x85)
  24  UINT8  attr_type     1 characteristic, 2 descriptor, 3 service
  25  UINT8  id
  26  UINT8  prop
  27  UINT8  is_primary

128-bit UUIDs are stored little-endian, matching the ATT value the
server copied during the traced discovery (tail 0xca9e).
"""

import struct
import sys

MESSAGE_SIZE = 296
HEADER_SIZE = 16
ATTR_SIZE = 28
MAX_ATTRS = 10

ATTR_CHAR = 1
ATTR_DESCR = 2
ATTR_SERVICE = 3

PROP_READ = 0x02
PROP_WRITE_NR = 0x04
PROP_WRITE = 0x08
PROP_NOTIFY = 0x10

NUS_BASE = "6e4000{:02x}-b5a3-f393-e0a9-e50e24dcca9e"
KETTLE_ADDRESS = bytes.fromhex("fb88f23934fb")


def uuid16(value):
    raw = bytearray(20)
    struct.pack_into("<H", raw, 0, 2)
    struct.pack_into("<H", raw, 4, value)
    return bytes(raw)


def uuid128(text):
    big = bytes.fromhex(text.replace("-", ""))
    if len(big) != 16:
        raise ValueError(text)
    raw = bytearray(20)
    struct.pack_into("<H", raw, 0, 16)
    raw[4:20] = big[::-1]
    return bytes(raw)


def nv_attr(uuid, start, end, attr_type, prop, is_primary, ident=0):
    record = bytearray(ATTR_SIZE)
    record[:20] = uuid
    struct.pack_into("<HHBBBB", record, 20, start, end, attr_type, ident, prop, is_primary)
    return bytes(record)


def kettle_attributes():
    """Services and the attributes the Ready for Sky path uses.

    Device Information ends at the manufacturer value 0x0011. The on-air
    discovery reported 000f–ffff, and that open end is what the crashing
    descriptor search walked. GAP characteristics are omitted; eight
    records fit in the single allowed batch.
    """
    return [
        nv_attr(uuid16(0x1800), 0x0001, 0x0007, ATTR_SERVICE, 0, 1),
        nv_attr(uuid16(0x1801), 0x0008, 0x0008, ATTR_SERVICE, 0, 1),
        nv_attr(uuid128(NUS_BASE.format(0x01)), 0x0009, 0x000E, ATTR_SERVICE, 0, 1),
        nv_attr(uuid128(NUS_BASE.format(0x03)), 0x000B, 0x000B, ATTR_CHAR, PROP_NOTIFY, 0),
        nv_attr(uuid16(0x2902), 0x000C, 0x000C, ATTR_DESCR, 0, 0),
        nv_attr(uuid128(NUS_BASE.format(0x02)), 0x000E, 0x000E, ATTR_CHAR, PROP_WRITE | PROP_WRITE_NR, 0),
        nv_attr(uuid16(0x180A), 0x000F, 0x0011, ATTR_SERVICE, 0, 1),
        nv_attr(uuid16(0x2A29), 0x0011, 0x0011, ATTR_CHAR, PROP_READ, 0),
    ]


def build_cache_load(conn_id, address=KETTLE_ADDRESS, attr_index=0):
    if not 0 <= conn_id <= 0xFFFF:
        raise ValueError(conn_id)
    if len(address) != 6:
        raise ValueError(address)
    attrs = kettle_attributes()
    if not attrs or len(attrs) > MAX_ATTRS:
        raise ValueError(len(attrs))
    message = bytearray(MESSAGE_SIZE)
    struct.pack_into("<HHHH", message, 0, 0, len(attrs), attr_index, conn_id)
    message[8:14] = address
    for index, record in enumerate(attrs):
        start = HEADER_SIZE + index * ATTR_SIZE
        message[start:start + ATTR_SIZE] = record
    return bytes(message)


def check(message, conn_id, address):
    if len(message) != MESSAGE_SIZE:
        raise AssertionError(len(message))
    more, num, index, loaded_conn = struct.unpack_from("<HHHH", message, 0)
    if (more, num, index, loaded_conn) != (0, 8, 0, conn_id):
        raise AssertionError((more, num, index, loaded_conn))
    if message[8:14] != address or message[14:16] != b"\x00\x00":
        raise AssertionError(message[8:16].hex())
    expected = kettle_attributes()
    for number, record in enumerate(expected):
        start = HEADER_SIZE + number * ATTR_SIZE
        if message[start:start + ATTR_SIZE] != record:
            raise AssertionError(number)
        uuid_len, = struct.unpack_from("<H", record, 0)
        handle, end, attr_type, ident, prop, primary = struct.unpack_from("<HHBBBB", record, 20)
        if uuid_len == 2 and struct.unpack_from("<H", record, 2) != (0,):
            raise AssertionError("uuid16 must sit at offset 4")
        if uuid_len not in (2, 16):
            raise AssertionError(uuid_len)
        if attr_type not in (ATTR_CHAR, ATTR_DESCR, ATTR_SERVICE):
            raise AssertionError(attr_type)
        if ident != 0:
            raise AssertionError(ident)
        del handle, end, prop, primary
    used = HEADER_SIZE + len(expected) * ATTR_SIZE
    if message[used:] != bytes(MESSAGE_SIZE - used):
        raise AssertionError("tail is not zero")
    dis = message[HEADER_SIZE + 6 * ATTR_SIZE:HEADER_SIZE + 7 * ATTR_SIZE]
    if struct.unpack_from("<H", dis, 22) != (0x0011,):
        raise AssertionError("180a end handle")
    if struct.unpack_from("<H", dis, 20) != (0x000F,):
        raise AssertionError("180a start handle")
    notify = message[HEADER_SIZE + 3 * ATTR_SIZE:HEADER_SIZE + 4 * ATTR_SIZE]
    if notify[4:20] != bytes.fromhex(NUS_BASE.format(0x03).replace("-", ""))[::-1]:
        raise AssertionError("notify uuid byte order")
        if struct.unpack_from("<HHB", notify, 20) != (0x000B, 0x000B, ATTR_CHAR) or notify[26] != PROP_NOTIFY:
            raise AssertionError("notify characteristic")
        write = message[HEADER_SIZE + 5 * ATTR_SIZE:HEADER_SIZE + 6 * ATTR_SIZE]
        if struct.unpack_from("<HHB", write, 20) != (0x000E, 0x000E, ATTR_CHAR):
            raise AssertionError("write characteristic range")


def main():
    conn_id = 5
    message = build_cache_load(conn_id)
    check(message, conn_id, KETTLE_ADDRESS)
    print("cache-load message: %d bytes, num_attr=8, more=0, attrs at offset 16" % len(message))
    print("180a end handle 0x0011; conn_id and address are filled by the caller")
    return 0


if __name__ == "__main__":
    sys.exit(main())
