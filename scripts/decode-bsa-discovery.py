#!/usr/bin/env python3
"""Print unique devices from the discovery probe's binary stdout."""
import json
import sys
from pathlib import Path

RECORD_SIZE = 256
data = Path(sys.argv[1]).read_bytes()
devices = {}
for offset in range(0, len(data), RECORD_SIZE):
    record = data[offset:offset + RECORD_SIZE]
    if len(record) != RECORD_SIZE or record[-1] != 10:
        sys.exit(f"Invalid discovery record at byte {offset}")
    address = record[:6].hex(":").upper()
    name = record[6:-1].split(b"\0", 1)[0].decode("utf-8", errors="replace")
    if name or address not in devices:
        devices[address] = name

for address, name in devices.items():
    print(address, json.dumps(name, ensure_ascii=False) if name else "(unnamed)")
print(f"{len(devices)} unique devices")
