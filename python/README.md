# Firmware Flash Tool (Python GUI)

Python/Tkinter GUI for loading, editing and transferring firmware binaries over a serial port.

## Installation

```powershell
pip install -r requirements.txt
python firmware_tool.py
```

Requires Python 3.8+ (Tkinter is included in the standard Windows installer) and `pyserial`.

## Features

- **Load firmware** (`*.bin`, `*.img`, `*.fw`, `*.rom`, or any file)
- **Hex editor** (offset | hex | ASCII), stays responsive even with large files
  - Click a byte and type hex digits (or type characters in the ASCII area)
  - `Tab` switches between hex and ASCII area; arrow keys / Page Up/Down / Home / End for navigation
  - Modified bytes are highlighted in **red**, `Ctrl+Z` = undo
  - "Bytes bearbeiten" (edit bytes) panel: jump to an offset or write multiple bytes (`DE AD BE EF`) starting at an offset
- **Info**: size, CRC32, 8-bit sum, number of modified bytes, value at cursor (hex/dec/bin/UInt16/UInt32)
- **Save / Save as**
- **Serial flashing**
  - Port selection (with refresh), baud rate, parity, stop bits
  - Protocols: `RAW` (configurable block size and delay per block), `XMODEM` (128 B, CRC/checksum), `XMODEM-1K`
  - Optional reset pulse via DTR/RTS and start delay
  - Progress bar, cancel button, log

> Note: The GUI labels are currently in German.

## Files

| File | Contents |
|---|---|
| `firmware_tool.py` | Main window / GUI |
| `hexview.py` | Hex editor widget |
| `flasher.py` | Serial transfer (RAW, XMODEM) |

## C command-line version

A dependency-free C implementation without GUI (Windows + Linux) is located in [`../c/`](../c/README.md).

## Extending

To support a device-specific bootloader protocol (e.g. STM32 UART bootloader, AVR/STK500, or a custom protocol),
add another `_send_...` method in `flasher.py` and add its name to `PROTOCOLS`.
