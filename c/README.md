# fwtool (C, command line) — v0.1.0

Command-line counterpart of the Python GUI: view and patch firmware binaries and transfer them over a serial port.
Runs on **Windows** and **Linux** (macOS should work as well). No external dependencies.

## Build

**Linux (gcc/clang):**
```sh
make
```

**Windows (MinGW-w64 / MSYS2):**
```powershell
mingw32-make
```

**Windows (Visual Studio) or any platform with CMake:**
```sh
cmake -B build
cmake --build build --config Release
```

**Without a build system:**
```sh
gcc -O2 -std=c99 -o fwtool src/*.c                                                # Linux
gcc -O2 -std=c99 -D__USE_MINGW_ANSI_STDIO=1 -o fwtool.exe src/*.c -ladvapi32     # MinGW
cl /O2 /D_CRT_SECURE_NO_WARNINGS src\*.c /Fe:fwtool.exe                          # MSVC
```

## Usage

All offsets and lengths are **hexadecimal** (`0x` prefix optional).

```text
fwtool info  <file>                              size, CRC32, 8-bit checksum
fwtool dump  <file> [offset] [length|all]        hex dump (default length 0x100)
fwtool patch <file> <offset> <hex...> [-o out]   overwrite bytes (in place or into a new file)
fwtool ports                                     list serial ports
fwtool flash <file> <port> [options]             transfer firmware
fwtool edit  <file>                              interactive editor / flasher
```

### Flash options

| Option | Description |
|---|---|
| `-b, --baud N` | baud rate (default 115200) |
| `-p, --protocol P` | `raw` \| `xmodem` \| `xmodem1k` (default `raw`) |
| `--parity X` | `n` \| `e` \| `o` (default `n`) |
| `--stop N` | stop bits `1` \| `2` |
| `-c, --chunk N` | RAW: bytes per block (default 256) |
| `-d, --delay MS` | RAW: pause after each block |
| `--start-delay MS` | wait before starting the transfer |
| `--reset` | pulse DTR/RTS before starting (resets many boards) |
| `--wait S` | XMODEM: seconds to wait for the receiver (default 60) |
| `--patch OFF=HEX` | patch bytes in memory before flashing (repeatable) |
| `-y, --yes` | skip confirmation prompt |

`Ctrl+C` cancels a running transfer (XMODEM sends CAN to the receiver).

### Examples

```sh
fwtool dump  firmware.bin 1F0 40
fwtool patch firmware.bin 0x1A0 DE AD BE EF -o patched.bin
fwtool flash firmware.bin COM3 -b 115200 -p xmodem
fwtool flash firmware.bin /dev/ttyUSB0 -b 921600 --reset --patch 1A0=DEADBEEF -y
```

### Interactive mode

```text
$ fwtool edit firmware.bin
fwtool> d 1A0 20            hex dump
fwtool> w 1A0 DE AD BE EF   write bytes (modified bytes are shown in red / marked with *)
fwtool> fill 200 10 FF      fill range with a byte value
fwtool> find 55 AA          search pattern
fwtool> u                   undo last write
fwtool> diff                list all changes
fwtool> s patched.bin       save
fwtool> ports               list serial ports
fwtool> flash COM3 -b 115200 -p xmodem1k
fwtool> q
```

## Notes

- **Linux:** your user needs access to the serial device, e.g. `sudo usermod -aG dialout $USER` (log in again afterwards).
- **Windows:** ports are named `COM1`, `COM3`, ... (COM10+ works too).
- Supported baud rates on Linux are the standard termios rates (up to 4000000).
- Colored output can be disabled with `--no-color` or the `NO_COLOR` environment variable.

## Source layout

| File | Contents |
|---|---|
| `src/fwtool.c` | command line, hex dump, patching, interactive mode |
| `src/flash.c/.h` | RAW / XMODEM / XMODEM-1K transfer, CRC16 |
| `src/serial.h` | serial port abstraction |
| `src/serial_win.c` | Windows implementation (Win32 API) |
| `src/serial_posix.c` | Linux/macOS implementation (termios) |
