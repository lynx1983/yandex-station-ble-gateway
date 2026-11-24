#!/usr/bin/env python3
"""Host regression tests for the Quasar voice client.

The tests compile the C modules for the host, connect them to a fake Quasar
listener on a private loopback port, and exercise the recognition and reply
paths without ARM tools or the BSA library. Unit tests for individual modules
live in tests/test_modules.c.
"""

import os
import shlex
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "src"
SOURCES = sorted(SOURCE_DIR.glob("*.c"))


def varint(value):
    out = bytearray()
    while value >= 128:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)
    return bytes(out)


def field(number, wire, payload=b"", value=None):
    key = varint((number << 3) | wire)
    if wire == 0:
        return key + varint(value)
    return key + varint(len(payload)) + payload


def recognition(text, final, request_id="id-1", mode=1):
    result = field(1, 2, text.encode("utf-8"))
    if final:
        result += field(2, 0, value=1)
    state = (
        field(1, 0, value=mode)
        + field(7, 2, request_id.encode("utf-8"))
        + field(15, 2, result)
    )
    payload = field(3, 2, state)
    return struct.pack("<I", len(payload)) + payload


def read_varint(data, offset):
    value = 0
    shift = 0
    while offset < len(data):
        byte = data[offset]
        offset += 1
        value |= (byte & 127) << shift
        if byte < 128:
            return value, offset
        shift += 7
        if shift > 63:
            raise ValueError("varint too long")
    raise ValueError("truncated varint")


def parse_fields(payload):
    fields = []
    offset = 0
    while offset < len(payload):
        key, offset = read_varint(payload, offset)
        number, wire = key >> 3, key & 7
        value, body = 0, b""
        if wire == 0:
            value, offset = read_varint(payload, offset)
        elif wire == 2:
            length, offset = read_varint(payload, offset)
            body = payload[offset : offset + length]
            offset += length
        elif wire == 1:
            body = payload[offset : offset + 8]
            offset += 8
        elif wire == 5:
            body = payload[offset : offset + 4]
            offset += 4
        else:
            raise ValueError("unsupported wire type %d" % wire)
        fields.append((number, wire, value, body))
    return fields


def find(fields, number, wire):
    for item in fields:
        if item[0] == number and item[1] == wire:
            return item
    return None


def parse_action(body):
    fields = parse_fields(body)
    wrapper = find(fields, 253, 0)
    if wrapper is not None and wrapper[2] == 210:
        remoting = find(fields, 252, 2)
    else:
        remoting = find(fields, 210, 2) or find(fields, 8, 2)
    if remoting is None:
        return None
    remoting_fields = parse_fields(remoting[3])
    action = find(remoting_fields, 8, 2)
    if action is None:
        return None
    action_fields = parse_fields(action[3])
    method = find(action_fields, 1, 0)
    if method is None:
        return None
    text = ""
    vocalize = find(action_fields, 13, 2)
    if vocalize is not None:
        spoken = find(parse_fields(vocalize[3]), 1, 2)
        if spoken is not None:
            text = spoken[3].decode("utf-8", "replace")
    return method[2], text


def parse_frames(data):
    messages = []
    offset = 0
    while len(data) - offset >= 4:
        size = struct.unpack_from("<I", data, offset)[0]
        if size > 65536:
            raise ValueError("oversized response frame")
        if len(data) - offset - 4 < size:
            break
        body = data[offset + 4 : offset + 4 + size]
        parsed = parse_action(body)
        if parsed is not None:
            messages.append(parsed)
        offset += 4 + size
    return messages, offset


class QuasarHarness:
    def __init__(self, defines=None, env_extra=None):
        self.tmp = tempfile.TemporaryDirectory(prefix="kettle-voice-test-")
        self.work = Path(self.tmp.name)
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(1)
        self.server.settimeout(5)
        self.port = self.server.getsockname()[1]
        self.binary = self.work / "alice-kettle"
        self.proc = None
        self.conn = None
        self.buffer = b""
        self.seen = []
        self.env_extra = env_extra or {}
        self._compile(defines or {})
        self.result = None

    def _compile(self, defines):
        compiler = shlex.split(os.environ.get("HOST_CC", "cc"))
        command = compiler + [
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I" + str(SOURCE_DIR),
            "-DQUASAR_PORT=%d" % self.port,
        ]
        command += ["-D%s=%s" % item for item in sorted(defines.items())]
        command += ["-o", str(self.binary)] + [str(path) for path in SOURCES]
        subprocess.run(command, check=True, capture_output=True)

    def write_worker(self, body, name="worker"):
        path = self.work / name
        path.write_text("#!/bin/sh\n" + body, encoding="utf-8")
        path.chmod(0o700)
        return path

    def start(self, worker):
        env = os.environ.copy()
        env.pop("KETTLE_PAIR_KEY", None)
        env["KETTLE_ACTIVITY_PATTERN"] = ""
        env.update(self.env_extra)
        self.proc = subprocess.Popen(
            [str(self.binary), str(worker), str(ROOT / "config/kettle-phrases.conf")],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            env=env,
        )
        self.conn, _ = self.server.accept()
        self.conn.settimeout(0.05)
        return self

    def send(self, text, final=False, request_id="id-1", mode=1):
        self.conn.sendall(recognition(text, final, request_id, mode))

    def _pump(self):
        try:
            chunk = self.conn.recv(65536)
        except socket.timeout:
            return
        except OSError:
            return
        if not chunk:
            return
        self.buffer += chunk
        messages, consumed = parse_frames(self.buffer)
        self.buffer = self.buffer[consumed:]
        self.seen.extend(messages)

    def wait_for(self, predicate, timeout=3.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self._pump()
            if any(predicate(message) for message in self.seen):
                return True
        return False

    def has(self, predicate):
        self._pump()
        return any(predicate(message) for message in self.seen)

    def finish(self):
        if self.conn is not None:
            self.conn.close()
        stderr = ""
        returncode = None
        if self.proc is not None:
            try:
                returncode = self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                returncode = self.proc.wait()
            stderr = self.proc.stderr.read().decode("utf-8", "replace")
            self.proc.stderr.close()
        self.server.close()
        self.result = (returncode, stderr)
        self.tmp.cleanup()
        return self.result

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        if self.result is None:
            self.finish()
        return False


def spoken(text):
    return lambda message: message[0] == 18 and text in message[1]


def wait_for_file(path, timeout=2.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return True
        time.sleep(0.02)
    return False


class VoiceClientTests(unittest.TestCase):
    def test_temperature_reply_and_cancellation(self):
        with QuasarHarness() as harness:
            worker = harness.write_worker("printf '52\\n'\n")
            harness.start(worker)
            harness.send("какая температура", final=False)
            time.sleep(0.05)
            harness.send("какая температура воды в чайнике", final=True)

            self.assertTrue(harness.wait_for(spoken("Температура воды в чайнике — 52 °C.")))
            self.assertTrue(harness.has(lambda message: message[0] == 6))
            self.assertTrue(harness.has(lambda message: message[0] == 14))

    def test_status_while_heating(self):
        with QuasarHarness() as harness:
            worker = harness.write_worker("printf '1 52 65\\n'\n")
            harness.start(worker)
            harness.send("какой статус", final=False)
            time.sleep(0.05)
            harness.send("какой статус чайника", final=True)

            self.assertTrue(harness.wait_for(spoken("нагревает воду до 65 °C. Сейчас примерно 52 °C.")))

    def test_duplicate_final_is_ignored(self):
        with QuasarHarness() as harness:
            worker = harness.write_worker("printf '48\\n'\n")
            harness.start(worker)
            harness.send("температура", final=False)
            time.sleep(0.05)
            frame = recognition("какая температура воды в чайнике", True)
            harness.conn.sendall(frame + frame)

            self.assertTrue(harness.wait_for(spoken("48 °C.")))
            time.sleep(0.15)
            replies = [message for message in harness.seen if message[0] == 18]
            self.assertEqual(len(replies), 1)

    def test_unknown_phrase_has_no_reply(self):
        with QuasarHarness() as harness:
            worker = harness.write_worker("printf 'called\\n' > \"$KETTLE_TEST_MARKER\"\n")
            marker = harness.work / "worker-called"
            harness.env_extra["KETTLE_TEST_MARKER"] = str(marker)
            harness.start(worker)
            harness.send("погода", final=False)
            time.sleep(0.05)
            harness.send("какая погода", final=True)
            time.sleep(0.2)

            self.assertFalse(harness.has(lambda message: message[0] == 18))
            self.assertFalse(harness.has(lambda message: message[0] == 6))

    def test_new_recognition_cycle_suppresses_stale_reply(self):
        with QuasarHarness() as harness:
            marker = harness.work / "worker-started"
            worker = harness.write_worker(
                "printf 'started\\n' > \"$KETTLE_TEST_MARKER\"\n"
                "sleep 0.4\n"
                "printf '52\\n'\n"
            )
            harness.env_extra["KETTLE_TEST_MARKER"] = str(marker)
            harness.start(worker)
            harness.send("температура", final=False)
            time.sleep(0.05)
            harness.send("какая температура воды в чайнике", final=True, request_id="first")
            self.assertTrue(wait_for_file(marker))
            harness.send("новый запрос", final=False, request_id="second", mode=1)
            time.sleep(0.6)

            self.assertFalse(harness.has(lambda message: message[0] == 18))

    def test_configured_thermos_phrase_passes_fixed_target(self):
        with QuasarHarness() as harness:
            worker = harness.write_worker(
                '[ "$#" -eq 2 ] && [ "$1" = heat ] && [ "$2" = 75 ] || exit 1\n'
                "printf 'started\\n'\n"
            )
            harness.start(worker)
            harness.send("нагрей воду", final=False)
            time.sleep(0.05)
            harness.send("нагрей воду в чайнике для термоса", final=True)

            self.assertTrue(harness.wait_for(
                spoken("Чайник начал нагрев до 75 °C."), timeout=1.5
            ))
            returncode, _ = harness.finish()
            self.assertEqual(returncode, 0)

    def test_invalid_heat_target_is_rejected_without_worker(self):
        with QuasarHarness() as harness:
            marker = harness.work / "worker-called"
            worker = harness.write_worker("printf 'called\\n' > \"$KETTLE_TEST_MARKER\"\n")
            harness.env_extra["KETTLE_TEST_MARKER"] = str(marker)
            harness.start(worker)
            harness.send("нагрей", final=False)
            time.sleep(0.05)
            harness.send("нагрей воду в чайнике до 20 градусов", final=True)

            expected = "Задайте целую температуру от 35 до 90 градусов."
            self.assertTrue(harness.wait_for(spoken(expected), timeout=1.5))
            self.assertFalse(marker.exists())
            returncode, _ = harness.finish()
            self.assertEqual(returncode, 0)

    def test_job_timeout_stops_worker_group(self):
        env_extra = {
            "KETTLE_JOB_TIMEOUT_MS": "300",
            "KETTLE_STOP_GRACE_MS": "100",
        }
        with QuasarHarness(env_extra=env_extra) as harness:
            pid_file = harness.work / "worker.pid"
            worker = harness.write_worker(
                "printf '%s\\n' \"$$\" > \"$KETTLE_TEST_PID\"\n"
                "exec sleep 311\n"
            )
            harness.env_extra["KETTLE_TEST_PID"] = str(pid_file)
            harness.start(worker)
            harness.send("температура", final=False)
            time.sleep(0.05)
            harness.send("какая температура воды в чайнике", final=True)

            expected = "Не удалось дождаться ответа от чайника"
            self.assertTrue(harness.wait_for(spoken(expected), timeout=2.0))
            self.assertTrue(pid_file.exists())
            worker_pid = int(pid_file.read_text(encoding="utf-8").strip())
            _, stderr = harness.finish()
            self.assertIn("time limit", stderr)
            self.assertIn("timed out", stderr)
            self.assertIn("Kettle job started: operation=temperature", stderr)
            self.assertIn("with SIGTERM", stderr)
            with self.assertRaises(ProcessLookupError):
                os.kill(worker_pid, 0)

    def test_missing_key_starts_pair_job(self):
        with QuasarHarness() as harness:
            key_file = harness.work / "pair.key"
            key_file.write_bytes(b"12345678")
            log = harness.work / "operations.log"
            worker = harness.write_worker(
                "printf '%s\\n' \"$1\" >> \"$KETTLE_TEST_LOG\"\n"
                "case \"$1\" in\n"
                "    temperature) exit 20 ;;\n"
                "    pair) exit 0 ;;\n"
                "esac\n"
                "exit 2\n"
            )
            harness.env_extra["KETTLE_PAIR_KEY"] = str(key_file)
            harness.env_extra["KETTLE_TEST_LOG"] = str(log)
            harness.start(worker)
            harness.send("ключ", final=False)
            time.sleep(0.05)
            harness.send("какая температура воды в чайнике", final=True)

            self.assertTrue(harness.wait_for(spoken("Чайник не подключен, давайте его подключим.")))
            self.assertTrue(harness.wait_for(spoken("Чайник привязан. Повторите нужную команду.")))
            operations = log.read_text(encoding="utf-8").split()
            self.assertEqual(operations, ["temperature", "pair"])
            _, stderr = harness.finish()
            self.assertIn("Starting pairing job after key-missing reply", stderr)
            self.assertIn("Kettle job started: operation=pair", stderr)
            self.assertIn("Pairing verified; waiting for a fresh command", stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
