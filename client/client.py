"""Простой Tkinter-клиент кодового замка STM32.

Запуск: python client.py [порт]  или  python client.py --port PORT
Tkinter: https://docs.python.org/3/library/tkinter.html
"""
import argparse
from collections import deque
from datetime import datetime
from pathlib import Path
from queue import Empty, Queue
import time
import tkinter as tk
from tkinter import ttk, messagebox
from tkinter.scrolledtext import ScrolledText

from serial.tools import list_ports
from connection import Connection


STATE_NAMES = {"INPUT": "Ввод кода", "EDIT": "Новый пароль",
               "CONFIRM_EDIT": "Подтвердите новый пароль", "OPEN": "Открыт",
               "BLOCKED": "Временно заблокирован", "FAULT": "Ошибка EEPROM"}
ERROR_NAMES = {"ERR BAD_CODE": "Неверный символ. Введите код заново.",
               "ERR BAD_CHAR": "Допустимы только английские буквы и цифры.",
               "ERR BAD_STATE": "Команда недоступна в текущем состоянии.",
               "ERR OPEN": "Замок уже открыт. Подождите 3 секунды.",
               "ERR BLOCKED": "Три ошибки: блокировка на 5 секунд.",
               "ERR EEPROM": "Ошибка EEPROM. Проверьте плату и перезапустите её.",
               "ERR UART": "Потеря данных UART. Подключитесь заново.",
               "ERR FRAME": "Ошибка кадра. Подключитесь заново.",
               "ERR COMMAND": "Прошивка не распознала команду."}


class LockClient:
    def __init__(self, root, port=""):
        self.root = root
        self.worker = None
        self.events = Queue()
        self.connected = False
        self.disconnecting = False
        self.busy = False
        self.active_command = None
        self.outgoing = deque()
        self.mode = "INPUT"
        self.deadline = None
        self.last_poll = 0.0
        self.expired = False
        self.log_file = None
        self.port = tk.StringVar(value=port)
        self.code = tk.StringVar()
        self.state = tk.StringVar(value="Не подключено")
        self.message = tk.StringVar(value="Выберите порт и нажмите «Подключиться».")
        self.countdown = tk.StringVar()
        self.log_location = tk.StringVar(value="Журнал сохраняется в папку logs.")
        self._build_ui()
        self.refresh_ports()
        self._controls()
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(30, self.process_events)

    def _build_ui(self):
        self.root.title("ПВС3 — кодовый замок")
        self.root.geometry("800x590")
        self.root.minsize(680, 520)
        frame = ttk.Frame(self.root, padding=12)
        frame.pack(fill="both", expand=True)
        frame.columnconfigure(0, weight=1)
        frame.rowconfigure(6, weight=1)

        connection = ttk.Frame(frame)
        connection.grid(row=0, column=0, sticky="ew", pady=(0, 12))
        connection.columnconfigure(1, weight=1)
        ttk.Label(connection, text="Порт:").grid(row=0, column=0, padx=(0, 6))
        self.port_box = ttk.Combobox(connection, textvariable=self.port, width=30)
        self.port_box.grid(row=0, column=1, sticky="ew")
        self.refresh_button = ttk.Button(connection, text="Обновить", command=self.refresh_ports)
        self.refresh_button.grid(row=0, column=2, padx=6)
        self.connect_button = ttk.Button(connection, text="Подключиться", command=self.toggle_connection)
        self.connect_button.grid(row=0, column=3)
        ttk.Label(frame, textvariable=self.state).grid(row=1, column=0, sticky="w")
        ttk.Label(frame, textvariable=self.message, wraplength=740).grid(row=2, column=0, sticky="w", pady=6)

        entry_row = ttk.Frame(frame)
        entry_row.grid(row=3, column=0, sticky="ew", pady=6)
        ttk.Label(entry_row, text="Код / новый пароль:").pack(side="left", padx=(0, 8))
        self.entry = ttk.Entry(entry_row, textvariable=self.code, width=18, font=("TkFixedFont", 18))
        self.entry.pack(side="left")
        self.entry.bind("<KeyPress>", self.on_key)
        self.entry.bind("<<Paste>>", self.paste)
        self.entry.bind("<<Cut>>", lambda event: "break")
        ttk.Label(entry_row, textvariable=self.countdown).pack(side="left", padx=12)

        actions = ttk.Frame(frame)
        actions.grid(row=4, column=0, sticky="w", pady=6)
        self.edit_button = ttk.Button(actions, text="Сменить пароль", command=lambda: self.control("EDIT"))
        self.edit_button.pack(side="left", padx=(0, 6))
        self.confirm_button = ttk.Button(actions, text="Подтвердить", command=lambda: self.control("CONFIRM"))
        self.confirm_button.pack(side="left", padx=6)
        self.cancel_button = ttk.Button(actions, text="Сбросить / отменить", command=lambda: self.control("CANCEL"))
        self.cancel_button.pack(side="left", padx=6)
        self.ping_button = ttk.Button(actions, text="Проверить связь", command=lambda: self.control("PING"))
        self.ping_button.pack(side="left", padx=6)
        ttk.Label(frame, text="8 английских букв или цифр. Символы отправляются сразу, Enter не нужен.\n"
                             "Backspace сбрасывает весь ввод. Для смены пароля нажмите «Сменить пароль».",
                  wraplength=740).grid(row=5, column=0, sticky="w", pady=6)
        self.log_widget = ScrolledText(frame, height=12, width=80, state="disabled", font=("TkFixedFont", 11))
        self.log_widget.grid(row=6, column=0, sticky="nsew", pady=6)
        ttk.Label(frame, textvariable=self.log_location, wraplength=740).grid(row=7, column=0, sticky="w")

    def refresh_ports(self):
        devices = [p.device for p in list_ports.comports()]
        # FTDI channel 1 — UART стенда; channel 0 используется отладчиком.
        devices.sort(key=lambda p: (not p.endswith("1"), p))
        self.port_box["values"] = devices
        if not self.port.get() and devices:
            self.port.set(devices[0])

    def _controls(self):
        def enable(widget, condition):
            widget.configure(state="normal" if condition else "disabled")
        idle = self.connected and not self.busy and not self.outgoing
        editable = self.connected and self.mode in ("INPUT", "EDIT") and not self.expired
        editable = editable and self.active_command not in ("EDIT", "CANCEL", "CONFIRM")
        enable(self.entry, editable)
        enable(self.edit_button, idle and self.mode == "INPUT")
        enable(self.confirm_button, idle and self.mode == "CONFIRM_EDIT")
        enable(self.cancel_button, idle and self.mode in ("INPUT", "EDIT", "CONFIRM_EDIT"))
        enable(self.ping_button, idle)
        enable(self.port_box, self.worker is None)
        enable(self.refresh_button, self.worker is None)
        enable(self.connect_button, not self.disconnecting)
        self.connect_button.configure(text="Отключиться" if self.worker else "Подключиться")

    def append_text(self, text):
        if not self.connected or self.mode not in ("INPUT", "EDIT") or self.expired:
            return
        if not text or any(not c.isascii() or not c.isalnum() for c in text):
            self.message.set("Используйте английскую раскладку: только A–Z, a–z и 0–9.")
            return
        if len(self.code.get()) + len(text) > 8:
            self.message.set("В коде ровно 8 символов. Для нового ввода нажмите «Сбросить».")
            return
        self.code.set(self.code.get() + text)
        self.entry.icursor("end")
        self.outgoing.extend("KEY " + c for c in text)
        self._pump()

    def on_key(self, event):
        if event.keysym in ("BackSpace", "Delete", "Escape"):
            if not self.busy and not self.outgoing:
                self.control("CANCEL")
            return "break"
        if event.keysym in ("Return", "KP_Enter"):
            if self.mode == "CONFIRM_EDIT":
                self.control("CONFIRM")
            return "break"
        # Комбинации Copy/Paste/Select All обрабатываются самим Tk.
        if event.state & 0x0C:  # Control / Command (Mod1 на macOS)
            return None
        if event.char and event.char.isprintable():
            self.append_text(event.char)
            return "break"
        return None

    def paste(self, _event=None):
        try:
            self.append_text(self.root.clipboard_get())
        except tk.TclError:
            pass
        return "break"

    def control(self, command):
        if not self.connected or self.busy or self.outgoing:
            return
        if command == "CONFIRM" and self.mode != "CONFIRM_EDIT":
            return
        self.outgoing.append(command)
        self._pump()

    def _pump(self):
        if not self.connected or self.busy or not self.outgoing:
            self._controls()
            return
        command = self.outgoing.popleft()
        if command.startswith("KEY ") and self.mode == "INPUT" and self.deadline is None:
            self.deadline = time.monotonic() + 5.0
        self.busy = True
        self.active_command = command
        self.worker.send(command)
        self._controls()

    def toggle_connection(self):
        if self.worker:
            self.disconnect()
            return
        if not self.port.get().strip():
            self.message.set("Укажите COM-порт платы.")
            return
        try:
            directory = Path(__file__).resolve().parent / "logs"
            directory.mkdir(exist_ok=True)
            path = directory / (datetime.now().strftime("%Y%m%d-%H%M%S-%f") + ".log")
            self.log_file = path.open("w", encoding="utf-8")
        except OSError as exc:
            messagebox.showerror("Журнал", str(exc), parent=self.root)
            return
        self.log_location.set(str(path))
        self.message.set("Подключение и проверка PING…")
        self.state.set("Подключение…")
        self.code.set("")
        self.outgoing.clear()
        self.busy = False
        self.active_command = None
        self.deadline = None
        self.expired = False
        self.events = Queue()
        self.worker = Connection(self.port.get().strip(), self.events)
        self.worker.start()
        self._controls()

    def disconnect(self):
        self.connected = False
        self.outgoing.clear()
        self.deadline = None
        self.busy = False
        if self.worker:
            self.disconnecting = True
            self.worker.stop()
        self.state.set("Не подключено")
        self._controls()

    def _log(self, stamp, direction, text):
        line = f"{stamp} {direction:4s} {text}\n"
        self.log_widget.configure(state="normal")
        self.log_widget.insert("end", line)
        if int(self.log_widget.index("end-1c").split(".")[0]) > 3000:
            self.log_widget.delete("1.0", "1000.0")
        self.log_widget.see("end")
        self.log_widget.configure(state="disabled")
        if self.log_file:
            try:
                self.log_file.write(line)
                self.log_file.flush()
            except OSError as exc:
                self.log_location.set(f"Ошибка записи журнала: {exc}")
                self.log_file.close()
                self.log_file = None

    def _state(self, new_mode):
        if new_mode != self.mode:
            if new_mode == "INPUT":
                self.code.set("")
                self.message.set("Введите код.")
            self.deadline = None
        self.mode = new_mode
        self.state.set(STATE_NAMES.get(new_mode, new_mode))

    def _reply(self, command, reply):
        self.busy = False
        self.active_command = None
        self.last_poll = time.monotonic()
        if reply.startswith("OK STATE "):
            self._state(reply.removeprefix("OK STATE "))
        elif reply == "OK OPEN":
            self._state("OPEN")
            self.outgoing.clear()
            self.expired = False
            self.message.set("PASSED — код принят.")
        elif reply == "OK CONFIRM_REQUIRED":
            self._state("CONFIRM_EDIT")
            self.deadline = time.monotonic() + 30
            self.message.set("Введены 8 символов. Нажмите «Подтвердить» для записи в EEPROM.")
        elif reply == "OK CHANGED":
            self._state("INPUT")
            self.code.set("")
            self.deadline = None
            self.message.set("Пароль сохранён в EEPROM.")
            self.entry.focus_set()
        elif reply == "OK" and command in ("EDIT", "CANCEL"):
            self._state("EDIT" if command == "EDIT" else "INPUT")
            self.code.set("")
            self.expired = False
            self.deadline = time.monotonic() + 30 if command == "EDIT" else None
            self.message.set("Введите новый пароль из 8 символов." if command == "EDIT" else "Ввод сброшен.")
            self.entry.focus_set()
        elif reply == "OK" and command.startswith("KEY "):
            if self.mode == "EDIT":
                self.deadline = time.monotonic() + 30
            self.message.set("Ввод…")
        elif reply == "PONG":
            self.message.set("Связь с платой есть — PONG.")
        elif reply.startswith("ERR "):
            self.outgoing.clear()
            self.deadline = None
            self.expired = False
            self.code.set("")
            self.message.set(ERROR_NAMES.get(reply, reply))
            if reply == "ERR BLOCKED":
                self._state("BLOCKED")
            elif reply == "ERR OPEN":
                self._state("OPEN")
            elif reply == "ERR EEPROM":
                self._state("FAULT")
            elif reply in ("ERR UART", "ERR FRAME", "ERR COMMAND"):
                self.disconnect()
            elif reply == "ERR BAD_STATE":
                self.outgoing.append("STATUS")
        self._controls()

    def process_events(self):
        # Все обращения к виджетам выполняются только в главном потоке Tk.
        try:
            while True:
                event = self.events.get_nowait()
                kind = event[0]
                if kind == "log":
                    self._log(*event[1:])
                elif kind == "connected" and not self.disconnecting:
                    self.connected = True
                    self._reply("STATUS", event[1])
                    if self.mode == "INPUT":
                        self.message.set("Подключено. Вводите код обычными клавишами.")
                    self.entry.focus_set()
                elif kind == "reply" and self.connected:
                    self._reply(event[1], event[2])
                elif kind == "error":
                    self.message.set(event[1] + " Закройте screen, если он использует этот порт.")
                    self.disconnect()
                elif kind == "closed":
                    self.connected = False
                    self.worker = None
                    self.disconnecting = False
                    self.busy = False
                    self.deadline = None
                    self.outgoing.clear()
                    self.state.set("Не подключено")
                    if self.log_file:
                        self.log_file.close()
                        self.log_file = None
        except Empty:
            pass
        now = time.monotonic()
        if self.connected and self.deadline is not None and now >= self.deadline and not self.expired:
            self.expired = True
            self.outgoing.clear()
            self.outgoing.append("CANCEL")
            self.code.set("")
            self.deadline = None
            self.message.set("Время ввода истекло. Сбрасываю ввод…")
        if self.deadline is not None:
            self.countdown.set(f"{len(self.code.get())}/8 · {max(0, self.deadline-now):.1f} с")
        else:
            self.countdown.set(f"{len(self.code.get())}/8")
        # Плата сама завершает OPEN/BLOCKED по таймеру — опрос нужен только
        # для этих переходов. В INPUT/EDIT не засоряем журнал постоянным STATUS.
        if (self.connected and self.mode in ("OPEN", "BLOCKED")
                and not self.busy and not self.outgoing
                and self.deadline is None and now-self.last_poll >= 1.0):
            self.outgoing.append("STATUS")
        self._pump()
        self.root.after(30, self.process_events)

    def close(self):
        if self.worker:
            self.worker.stop()
        if self.log_file:
            self.log_file.close()
        self.root.destroy()


def main():
    parser = argparse.ArgumentParser(description="Tkinter-клиент кодового замка STM32")
    parser.add_argument("port", nargs="?", help="например /dev/cu.usbserial-1101 или COM3")
    parser.add_argument("--port", dest="named_port", help="COM-порт платы")
    args = parser.parse_args()
    root = tk.Tk()
    LockClient(root, args.named_port or args.port or "")
    root.mainloop()


if __name__ == "__main__":
    main()
