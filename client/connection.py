"""UART в отдельном потоке. Tkinter получает события через Queue и root.after.

Протокол платы — Core/Src/lock_protocol.c проекта lab3-pvc.
Документация pySerial: https://pyserial.readthedocs.io/en/latest/pyserial_api.html
"""
from datetime import datetime
import os
from queue import Empty, Queue
import threading
import time

import serial


class Connection(threading.Thread):
    """В каждый момент отправлена только одна команда; далее ждём её ответ."""

    def __init__(self, port, events, serial_factory=serial.Serial):
        super().__init__(daemon=True)
        self.port = port
        self.events = events
        self.commands = Queue()
        self.stopped = threading.Event()
        self.serial_factory = serial_factory
        self.device = None
        self.rx = bytearray()

    def log(self, direction, text):
        stamp = datetime.now().astimezone().isoformat(timespec="milliseconds")
        self.events.put(("log", stamp, direction, text))

    def stop(self):
        self.stopped.set()

    def send(self, command):
        self.commands.put(command)

    def _write(self, packet, label):
        self.log("TX", label)
        if self.device.write(packet) != len(packet):
            raise OSError("Команда передана не полностью. Подключитесь заново.")

    def _line(self, deadline):
        """Сохраняет неполную строку между чтениями; EVT не является ответом."""
        while not self.stopped.is_set() and time.monotonic() < deadline:
            if b"\n" in self.rx:
                raw, _, rest = self.rx.partition(b"\n")
                self.rx = bytearray(rest)
                line = raw.rstrip(b"\r").decode("ascii", errors="replace")
                if line:
                    self.log("RX", line)
                    if line.startswith("EVT "):
                        self.events.put(("event", line))
                        continue
                    return line
            data = self.device.read(128)
            self.rx.extend(data)
            if len(self.rx) > 512:
                raise OSError("Слишком длинный ответ платы. Проверьте скорость 9600.")
        return None

    def request(self, command):
        self._write((command + "\r\n").encode("ascii"), command)
        line = self._line(time.monotonic() + 2.0)
        if self.stopped.is_set():
            return None
        if line is None:
            # Не повторяем команду: первый запрос мог уже изменить состояние замка.
            raise TimeoutError(f"Нет ответа на {command!r}. Проверьте плату и порт.")
        if command == "PING":
            allowed = {"PONG"}
        elif command == "STATUS":
            allowed = {"OK STATE " + state for state in
                       ("INPUT", "EDIT", "CONFIRM_EDIT", "OPEN", "BLOCKED")}
            allowed.add("ERR EEPROM")
        else:
            allowed = {"OK", "OK OPEN", "OK CONFIRM_REQUIRED", "OK CHANGED",
                       "ERR BAD_CODE", "ERR BAD_CHAR", "ERR BAD_STATE", "ERR OPEN",
                       "ERR BLOCKED", "ERR EEPROM", "ERR UART", "ERR COMMAND", "ERR FRAME"}
        if line not in allowed:
            raise OSError(f"Неожиданный ответ {line!r} на {command!r}. Подключитесь заново.")
        return line

    def run(self):
        try:
            options = dict(port=self.port, baudrate=9600, bytesize=serial.EIGHTBITS,
                           parity=serial.PARITY_NONE, stopbits=serial.STOPBITS_ONE,
                           timeout=0.05, write_timeout=0.5,
                           xonxoff=False, rtscts=False, dsrdtr=False)
            if os.name == "posix":
                options["exclusive"] = True
            self.device = self.serial_factory(**options)
            # Esc выводит из TERMINAL. В режиме команд он даёт ERR FRAME, который
            # здесь ожидаем. CR/LF завершает также недописанную команду/resync.
            self.device.reset_input_buffer()
            self._write(b"\x1b\r\n", "<Esc><CR><LF> (синхронизация)")
            until = time.monotonic() + 0.4
            while not self.stopped.is_set() and time.monotonic() < until:
                self._line(until)
            if self.stopped.is_set():
                return
            if self.request("PING") != "PONG":
                return
            self.request("CANCEL")  # Новое подключение начинает новый ввод.
            state = self.request("STATUS")
            if self.stopped.is_set():
                return
            self.events.put(("connected", state))
            while not self.stopped.is_set():
                try:
                    command = self.commands.get(timeout=0.05)
                except Empty:
                    line = self._line(time.monotonic() + 0.05)
                    if line is not None:
                        raise OSError(f"Ответ без запроса: {line!r}. Подключитесь заново.")
                    continue
                reply = self.request(command)
                if reply is not None:
                    self.events.put(("reply", command, reply))
        except (serial.SerialException, OSError, ValueError) as exc:
            if not self.stopped.is_set():
                self.events.put(("error", str(exc)))
        finally:
            if self.device is not None:
                self.device.close()
            self.events.put(("closed",))
