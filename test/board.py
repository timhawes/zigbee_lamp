"""Serial helper for driving ESP32 boards running an esp_console REPL."""

import re
import threading
import time

import serial

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]|\x1b\[\d*n")


class Board:
    def __init__(self, name, port, log_file=None):
        self.name = name
        # Note: on Linux, opening the USB-Serial/JTAG port resets the chip anyway.
        self.ser = serial.Serial(None, 115200, timeout=0.1)
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.port = port
        self.ser.open()
        self.lines = []          # every line received, in order
        self.cursor = 0          # index of first line not yet consumed by expect()
        self.lock = threading.Lock()
        self.log_file = open(log_file, "a") if log_file else None
        self._partial = ""
        self._stop = False
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def _reader(self):
        while not self._stop:
            try:
                data = self.ser.read(4096)
            except serial.SerialException:
                time.sleep(0.2)   # port vanishes briefly across a USB-JTAG reset
                continue
            if not data:
                continue
            text = self._partial + ANSI.sub("", data.decode(errors="replace"))
            parts = re.split(r"\r?\n", text)
            self._partial = parts.pop()
            with self.lock:
                for line in parts:
                    line = line.replace("\r", "")
                    self.lines.append(line)
                    if self.log_file:
                        self.log_file.write(f"{time.time():.3f} [{self.name}] {line}\n")
                        self.log_file.flush()

    def wait_ready(self, timeout=10.0):
        """Wait for the console to come up after a reset."""
        self.expect(r"Returned from app_main", timeout)
        time.sleep(0.2)

    def close(self):
        self._stop = True
        self._thread.join(1)
        self.ser.close()

    def reset(self):
        """Hardware reset through the USB-Serial/JTAG RTS line (like esptool's hard_reset)."""
        self.ser.dtr = False
        self.ser.rts = True
        time.sleep(0.1)
        self.ser.rts = False

    def skip(self):
        """Mark all received lines as consumed."""
        with self.lock:
            self.cursor = len(self.lines)

    def send(self, cmd, skip=True):
        if skip:
            self.skip()
        self.ser.write((cmd + "\n").encode())
        self.ser.flush()

    def expect(self, pattern, timeout=10.0):
        """Wait for a line matching pattern after the cursor; returns the match and advances the cursor."""
        rx = re.compile(pattern)
        end = time.time() + timeout
        while True:
            with self.lock:
                while self.cursor < len(self.lines):
                    line = self.lines[self.cursor]
                    self.cursor += 1
                    m = rx.search(line)
                    if m:
                        return m
            if time.time() > end:
                raise TimeoutError(f"[{self.name}] timed out waiting for /{pattern}/")
            time.sleep(0.05)

    def lines_after(self, mark):
        with self.lock:
            return list(self.lines[mark:])

    def mark(self):
        with self.lock:
            return len(self.lines)
