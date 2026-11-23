#!/usr/bin/env python3
"""Validate ARM assembly includes and conditional balance without binutils."""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FILES = [
    "scripts/bsa-management-probe.S",
    "scripts/bsa-gatt-search.inc",
    "scripts/kettle-cache-attrs.inc",
]
INCLUDE = re.compile(r'^\s*\.include\s+"([^"]+)"')
OPENERS = {".ifdef", ".ifndef", ".if"}


def main():
    errors = []
    for name in FILES:
        path = ROOT / name
        depth = 0
        for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            line = raw.split("@", 1)[0].strip()
            match = INCLUDE.match(line)
            if match:
                target = ROOT / match.group(1)
                if not target.is_file():
                    errors.append(f"{name}:{number}: missing include {match.group(1)}")
            directive = line.split(None, 1)[0] if line.startswith(".") else ""
            if directive in OPENERS:
                depth += 1
            elif directive == ".endif":
                depth -= 1
                if depth < 0:
                    errors.append(f"{name}:{number}: unmatched .endif")
                    depth = 0
            elif directive in {".else", ".elseif"} and depth == 0:
                errors.append(f"{name}:{number}: {directive} outside a conditional")
        if depth:
            errors.append(f"{name}: {depth} unclosed conditional(s)")

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print("assembly includes and conditionals are consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
