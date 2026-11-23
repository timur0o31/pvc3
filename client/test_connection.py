"""Проверка транспорта без платы: python -m unittest -v."""
from queue import Queue
import threading
import time
import unittest

from connection import Connection


class FakeSerial:
    def __init__(self, terminal=False, **options):
        assert options["baudrate"] == 9600
        assert options["timeout"] == 0.05
        self.terminal = terminal
        self.received = []
        self.incoming = bytearray()
        self.closed = False
        self.guard = threading.Lock()
        self.silent = False
        self.failure = False
        self.partial = False

    def reset_input_buffer(self):
        with self.guard:
            self.incoming.clear()

    def write(self, packet):
        self.received.append(packet)
        if self.failure:
            raise OSError("USB disconnected")
        if self.partial:
            return len(packet) - 1
        if self.silent:
            return len(packet)
        if packet == b"\x1b\r\n":
            reply = b"OK COMMANDS\r\n" if self.terminal else b"ERR FRAME\r\n"
            self.terminal = False
        elif packet == b"PING\r\n":
            reply = b"EVT UART IRQ\r\nPONG\r\n"
        elif packet == b"STATUS\r\n":
            reply = b"OK STATE INPUT\r\n"
        elif packet == b"KEY X\r\n":
            reply = b"ERR BAD_CODE\r\n"
        else:
            reply = b"OK\r\n"
        with self.guard:
            self.incoming.extend(reply)
        return len(packet)

    def read(self, _size):
        if self.failure:
            raise OSError("USB disconnected")
        with self.guard:
            # Намеренно разбиваем даже CR/LF и EVT между чтениями.
            data = self.incoming[:2]
            del self.incoming[:2]
        if not data:
            time.sleep(0.005)
        return bytes(data)

    def close(self):
        self.closed = True


class ConnectionTests(unittest.TestCase):
    def start_connection(self, terminal=False):
        events = Queue()
        holder = []
        def factory(**options):
            port = FakeSerial(terminal=terminal, **options)
            holder.append(port)
            return port
        worker = Connection("TEST", events, factory)
        worker.start()
        seen = []
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            event = events.get(timeout=2)
            seen.append(event)
            if event[0] == "connected":
                self.assertEqual(event[1], "OK STATE INPUT")
                break
        else:
            self.fail("No connected event")
        self.addCleanup(self.stop_connection, worker)
        return worker, holder[0], events, seen

    @staticmethod
    def stop_connection(worker):
        worker.stop()
        worker.join(timeout=1)
        assert not worker.is_alive()

    def until(self, events, kind, timeout=3):
        deadline = time.monotonic() + timeout
        seen = []
        while time.monotonic() < deadline:
            event = events.get(timeout=timeout)
            seen.append(event)
            if event[0] == kind:
                return event, seen
        self.fail(f"Missing {kind}")

    def test_handshake_from_both_modes_and_fragmented_events(self):
        for terminal in (False, True):
            with self.subTest(terminal=terminal):
                worker, port, events, seen = self.start_connection(terminal)
                self.assertEqual(port.received[:4], [b"\x1b\r\n", b"PING\r\n", b"CANCEL\r\n", b"STATUS\r\n"])
                self.assertIn(("event", "EVT UART IRQ"), seen)
                self.assertTrue(any(e[0] == "log" and e[2] == "TX" for e in seen))
                worker.send("KEY A")
                self.assertEqual(self.until(events, "reply")[0], ("reply", "KEY A", "OK"))
                worker.send("KEY X")
                self.assertEqual(self.until(events, "reply")[0], ("reply", "KEY X", "ERR BAD_CODE"))
                self.stop_connection(worker)
                self.assertTrue(port.closed)

    def test_timeout_does_not_repeat_command(self):
        worker, port, events, _ = self.start_connection()
        port.silent = True
        worker.send("CONFIRM")
        error, _ = self.until(events, "error")
        self.assertIn("Нет ответа", error[1])
        self.until(events, "closed")
        self.assertEqual(port.received.count(b"CONFIRM\r\n"), 1)
        self.assertTrue(port.closed)

    def test_disconnect_during_request(self):
        worker, port, events, _ = self.start_connection()
        port.failure = True
        worker.send("PING")
        self.assertIn("USB disconnected", self.until(events, "error")[0][1])
        self.until(events, "closed")
        self.assertTrue(port.closed)

    def test_short_write_closes_connection(self):
        worker, port, events, _ = self.start_connection()
        port.partial = True
        worker.send("KEY A")
        self.assertIn("не полностью", self.until(events, "error")[0][1])
        self.until(events, "closed")
        self.assertTrue(port.closed)


if __name__ == "__main__":
    unittest.main()
