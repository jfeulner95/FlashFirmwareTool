"""Protocol tests for flasher.py against a simulated serial receiver (no hardware needed)."""

import threading

import pytest

from flasher import ACK, CAN, EOT, NAK, SOH, STX, FlashCancelled, Flasher, crc16_xmodem


class FakeSerial:
    """Minimal stand-in for serial.Serial that acts like an XMODEM receiver.

    `initial` is what the receiver has already sent before the transfer starts
    (real receivers repeat 'C' or NAK about once per second while waiting).
    `responses` maps a packet number (1-based, in send order) to the byte the
    receiver answers with; all other packets are ACKed.
    """

    def __init__(self, initial=b"C", responses=None, silent=False):
        self.rx = bytearray(initial)
        self.responses = responses or {}
        self.silent = silent
        self.sent = []
        self.timeout = 1

    def reset_input_buffer(self):
        self.rx.clear()

    def write(self, data):
        data = bytes(data)
        self.sent.append(data)
        if self.silent or data[0] not in (SOH, STX, EOT):
            return len(data)
        n = sum(1 for p in self.sent if p[0] in (SOH, STX))
        self.rx.append(self.responses.get(n, ACK) if data[0] != EOT else ACK)
        return len(data)

    def flush(self):
        pass

    def read(self, n=1):
        if self.rx:
            out = bytes(self.rx[:n])
            del self.rx[:n]
            return out
        return b""

    def blocks(self):
        return [p for p in self.sent if p[0] in (SOH, STX)]


def test_crc16_xmodem_known_value():
    # Standard check value for CRC-16/XMODEM
    assert crc16_xmodem(b"123456789") == 0x31C3
    assert crc16_xmodem(b"") == 0


def test_xmodem_crc_blocks_and_padding():
    data = bytes(range(256)) + b"\x01\x02\x03"  # 259 bytes -> 3 blocks of 128
    ser = FakeSerial(initial=b"C")
    Flasher("X", 115200, "XMODEM")._send_xmodem(ser, data, 128)

    blocks = ser.blocks()
    assert len(blocks) == 3
    for i, pkt in enumerate(blocks, start=1):
        assert pkt[0] == SOH
        assert pkt[1] == i and pkt[2] == 0xFF - i
        assert len(pkt) == 3 + 128 + 2
    last_payload = blocks[-1][3:-2]
    assert last_payload[:3] == b"\x01\x02\x03"
    assert set(last_payload[3:]) == {0x1A}  # padded with SUB
    assert int.from_bytes(blocks[0][-2:], "big") == crc16_xmodem(data[:128])
    assert ser.sent[-1] == bytes([EOT])


def test_xmodem_queued_handshake_bytes_do_not_duplicate_block_1():
    """Regression: extra 'C's queued by the receiver were read as NAK for block 1."""
    ser = FakeSerial(initial=b"CCC")
    Flasher("X", 115200, "XMODEM")._send_xmodem(ser, bytes(512), 128)
    assert [p[1] for p in ser.blocks()] == [1, 2, 3, 4]


def test_xmodem_checksum_mode_and_1k_fallback():
    data = bytes(range(200))
    ser = FakeSerial(initial=bytes([NAK]))
    logs = []
    Flasher("X", 115200, "XMODEM-1K", log=logs.append)._send_xmodem(ser, data, 1024)

    blocks = ser.blocks()
    assert all(p[0] == SOH and len(p) == 3 + 128 + 1 for p in blocks)  # 128-byte blocks, 1-byte sum
    assert blocks[0][-1] == sum(data[:128]) & 0xFF
    assert any("Fallback" in m for m in logs)


def test_xmodem_1k_uses_stx():
    ser = FakeSerial(initial=b"C")
    Flasher("X", 115200, "XMODEM-1K")._send_xmodem(ser, bytes(1500), 1024)
    assert [len(p) for p in ser.blocks()] == [3 + 1024 + 2] * 2
    assert all(p[0] == STX for p in ser.blocks())


def test_xmodem_nak_triggers_retransmit():
    ser = FakeSerial(initial=b"C", responses={1: NAK})
    Flasher("X", 115200, "XMODEM")._send_xmodem(ser, bytes(128), 128)
    assert [p[1] for p in ser.blocks()] == [1, 1]


def test_xmodem_receiver_cancel_raises():
    ser = FakeSerial(initial=b"C", responses={1: CAN})
    with pytest.raises(RuntimeError, match="CAN"):
        Flasher("X", 115200, "XMODEM")._send_xmodem(ser, bytes(128), 128)


def test_xmodem_user_cancel_is_fast_and_sends_can():
    """Cancel must be honored while waiting for an answer, not only after the 10 s timeout."""
    ser = FakeSerial(initial=b"C", silent=True)
    f = Flasher("X", 115200, "XMODEM")
    threading.Timer(0.3, f.cancel).start()
    with pytest.raises(FlashCancelled):
        f._send_xmodem(ser, bytes(128), 128)
    assert ser.sent[-1] == bytes([CAN] * 3)


def test_raw_chunks_and_progress():
    progress = []
    ser = FakeSerial(initial=b"")
    f = Flasher("X", 115200, "RAW", chunk_size=100, progress=lambda d, t: progress.append((d, t)))
    f._send_raw(ser, bytes(250))
    assert [len(p) for p in ser.sent] == [100, 100, 50]
    assert progress == [(100, 250), (200, 250), (250, 250)]
