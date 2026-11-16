"""Übertragung eines Firmware-Images über eine serielle Schnittstelle.

Unterstützte Protokolle:
  * RAW        – Bytes werden blockweise unverändert gesendet
  * XMODEM     – 128-Byte-Blöcke, CRC16 (Fallback: Checksumme)
  * XMODEM-1K  – 1024-Byte-Blöcke, CRC16
"""

import threading
import time

import serial
import serial.tools.list_ports

SOH, STX, EOT, ACK, NAK, CAN = 0x01, 0x02, 0x04, 0x06, 0x15, 0x18
CRC_REQ = ord("C")

PROTOCOLS = ["RAW", "XMODEM", "XMODEM-1K"]
PARITIES = {"Keine (N)": serial.PARITY_NONE, "Gerade (E)": serial.PARITY_EVEN, "Ungerade (O)": serial.PARITY_ODD}
STOPBITS = {"1": serial.STOPBITS_ONE, "2": serial.STOPBITS_TWO}


class FlashCancelled(Exception):
    pass


def list_ports():
    """Liefert [(device, beschreibung), ...] aller verfügbaren seriellen Ports."""
    ports = sorted(serial.tools.list_ports.comports(), key=lambda p: p.device)
    return [(p.device, p.description) for p in ports]


def crc16_xmodem(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


class Flasher:
    def __init__(self, port, baudrate, protocol="RAW", parity=serial.PARITY_NONE,
                 stopbits=serial.STOPBITS_ONE, chunk_size=256, chunk_delay_ms=0,
                 dtr_reset=False, start_delay_ms=0, progress=None, log=None):
        self.port = port
        self.baudrate = baudrate
        self.protocol = protocol
        self.parity = parity
        self.stopbits = stopbits
        self.chunk_size = max(1, chunk_size)
        self.chunk_delay = max(0, chunk_delay_ms) / 1000.0
        self.dtr_reset = dtr_reset
        self.start_delay = max(0, start_delay_ms) / 1000.0
        self.progress = progress or (lambda done, total: None)
        self.log = log or (lambda msg: None)
        self._cancel = threading.Event()

    def cancel(self):
        self._cancel.set()

    def _check_cancel(self):
        if self._cancel.is_set():
            raise FlashCancelled()

    # ----------------------------------------------------------------- run
    def run(self, data: bytes):
        self.log(f"Öffne {self.port} @ {self.baudrate} Baud ({self.protocol})")
        with serial.Serial(self.port, self.baudrate, bytesize=serial.EIGHTBITS,
                           parity=self.parity, stopbits=self.stopbits,
                           timeout=1, write_timeout=10) as ser:
            if self.dtr_reset:
                self.log("Reset-Impuls über DTR/RTS")
                ser.dtr = False
                ser.rts = False
                time.sleep(0.1)
                ser.dtr = True
                ser.rts = True
                time.sleep(0.1)
            if self.start_delay:
                self.log(f"Warte {self.start_delay * 1000:.0f} ms vor Start")
                self._sleep(self.start_delay)
            ser.reset_input_buffer()

            t0 = time.time()
            if self.protocol == "RAW":
                self._send_raw(ser, data)
            else:
                self._send_xmodem(ser, data, 1024 if self.protocol == "XMODEM-1K" else 128)
            dt = time.time() - t0
            rate = len(data) / dt / 1024 if dt > 0 else 0
            self.log(f"Fertig: {len(data)} Bytes in {dt:.2f} s ({rate:.1f} KiB/s)")

    def _sleep(self, seconds):
        end = time.time() + seconds
        while True:
            self._check_cancel()
            remaining = end - time.time()
            if remaining <= 0:
                break
            time.sleep(min(0.05, remaining))

    # ----------------------------------------------------------------- RAW
    def _send_raw(self, ser, data):
        total = len(data)
        for off in range(0, total, self.chunk_size):
            self._check_cancel()
            ser.write(data[off:off + self.chunk_size])
            ser.flush()
            self.progress(min(off + self.chunk_size, total), total)
            if self.chunk_delay:
                time.sleep(self.chunk_delay)

    # -------------------------------------------------------------- XMODEM
    def _send_xmodem(self, ser, data, block_size):
        total = len(data)
        self.log("Warte auf Empfänger (‘C’ oder NAK) ... max. 60 s")
        use_crc = None
        deadline = time.time() + 60
        ser.timeout = 1
        while use_crc is None:
            try:
                self._check_cancel()
            except FlashCancelled:
                ser.write(bytes([CAN, CAN]))
                raise
            if time.time() > deadline:
                raise TimeoutError("Empfänger hat nicht geantwortet")
            c = ser.read(1)
            if not c:
                continue
            if c[0] == CRC_REQ:
                use_crc = True
            elif c[0] == NAK:
                use_crc = False
            elif c[0] == CAN:
                raise RuntimeError("Empfänger hat abgebrochen (CAN)")
        if not use_crc and block_size == 1024:
            self.log("Empfänger unterstützt kein CRC -> Fallback auf 128-Byte-Blöcke")
            block_size = 128
        self.log(f"Empfänger bereit, Modus: {'CRC16' if use_crc else 'Checksumme'}, Block {block_size} B")

        ser.timeout = 10
        blk = 1
        off = 0
        while off < total:
            chunk = data[off:off + block_size]
            if len(chunk) < block_size:
                chunk += b"\x1A" * (block_size - len(chunk))
            header = STX if block_size == 1024 else SOH
            pkt = bytes([header, blk & 0xFF, 0xFF - (blk & 0xFF)]) + chunk
            pkt += crc16_xmodem(chunk).to_bytes(2, "big") if use_crc else bytes([sum(chunk) & 0xFF])

            for attempt in range(10):
                if self._cancel.is_set():
                    ser.write(bytes([CAN, CAN]))
                    raise FlashCancelled()
                ser.write(pkt)
                r = ser.read(1)
                if r and r[0] == ACK:
                    break
                if r and r[0] == CAN:
                    raise RuntimeError("Empfänger hat abgebrochen (CAN)")
                self.log(f"Block {blk}: {'NAK' if r else 'Timeout'} – Wiederholung {attempt + 1}/10")
            else:
                ser.write(bytes([CAN, CAN]))
                raise RuntimeError(f"Block {blk}: zu viele Fehler, Abbruch")

            off += block_size
            blk += 1
            self.progress(min(off, total), total)

        for attempt in range(10):
            ser.write(bytes([EOT]))
            r = ser.read(1)
            if r and r[0] == ACK:
                return
        raise RuntimeError("EOT wurde nicht bestätigt")
