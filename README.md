# Firmware Flash Tool

[![CI](https://github.com/jfeulner95/FlashFirmwareTool/actions/workflows/ci.yml/badge.svg)](https://github.com/jfeulner95/FlashFirmwareTool/actions/workflows/ci.yml)
[![Lint & Style](https://github.com/jfeulner95/FlashFirmwareTool/actions/workflows/lint.yml/badge.svg)](https://github.com/jfeulner95/FlashFirmwareTool/actions/workflows/lint.yml)
[![CodeQL](https://github.com/jfeulner95/FlashFirmwareTool/actions/workflows/codeql.yml/badge.svg)](https://github.com/jfeulner95/FlashFirmwareTool/actions/workflows/codeql.yml)

![Firmware Flash Tool Social Preview](assets/social-preview.png)

**Version:** `0.1.0`

**Firmware Flash Tool** is a lightweight toolkit for inspecting, patching, and transferring microcontroller firmware binaries over a serial port. It includes two implementations with matching capabilities:

1. **[Python GUI (`python/`)](python/README.md)** — A desktop graphical user interface built with `tkinter` and `pyserial`.
2. **[C CLI (`c/`)](c/README.md)** — A dependency-free, cross-platform command-line and interactive terminal tool written in C99 for Windows and Linux.

---

## Overview of Both Tools

### 1. Python GUI (`python/`) — `v0.1.0`

A graphical application for interactive firmware inspection, live byte editing, and serial flashing.

- **Virtualized Hex Editor:** Displays offset, hexadecimal bytes, and ASCII representation side by side. Only visible rows are rendered on the canvas, keeping the editor fast and responsive even with multi-megabyte firmware images.
- **In-Place Editing & Patching:** Edit single bytes directly in the hex or ASCII pane (with `Ctrl+Z` undo and red highlighting for modified bytes), or write multi-byte hex sequences at any target offset.
- **Live Checksums & Inspector:** Automatically computes file size, `CRC32`, 8-bit sum, modified byte count, and decodes the value at the cursor (`Hex`, `Dec`, `Bin`, `UInt16 LE`, `UInt32 LE`).
- **Serial Flashing:** Transfers the current editor buffer over any serial port using **RAW**, **XMODEM** (128-byte blocks, CRC16/checksum), or **XMODEM-1K** (1024-byte blocks) with configurable baud rate, parity, stop bits, chunk size/delay, and optional `DTR`/`RTS` target reset pulse.

👉 **Full documentation & setup:** **[`python/README.md`](python/README.md)**

---

### 2. C Command-Line Tool (`c/`) — `v0.1.0`

A fast, standalone command-line utility (`fwtool`) with zero external dependencies, using the native Win32 Comm API on Windows and `termios` on Linux/macOS.

- **Non-Interactive CLI Commands:**
  - `fwtool info <file>` — Display file size, `CRC32`, and 8-bit checksum.
  - `fwtool dump <file> [offset] [length|all]` — Colored hex + ASCII dump.
  - `fwtool patch <file> <offset> <hex...> [-o out.bin]` — Patch bytes in-place or write to a new file.
  - `fwtool ports` — Enumerate available serial ports.
  - `fwtool flash <file> <port> [options]` — Flash a binary (supports on-the-fly in-memory patching via `--patch OFF=HEX`, `DTR`/`RTS` reset, and **RAW** / **XMODEM** / **XMODEM-1K**).
- **Interactive Shell (`fwtool edit <file>`):** Load a binary into memory, inspect hex dumps (`d`), write bytes (`w`), fill ranges (`fill`), search patterns (`find`), inspect diffs (`diff`), undo changes (`u`), save (`s`), and flash directly from the prompt (`flash`).

👉 **Full documentation & build instructions:** **[`c/README.md`](c/README.md)**

---

## Repository Structure

```text
FlashFirmwareTool/
├── .github/
│   ├── workflows/           # CI, lint/style, CodeQL, Claude code review
│   └── dependabot.yml       # Weekly updates for actions and pip
├── assets/
│   ├── icon.png             # Application icon (512x512 PNG)
│   ├── icon.ico             # Windows application icon (ICO)
│   └── social-preview.png   # GitHub social preview banner (1280x640)
├── python/                  # Python/Tkinter GUI (v0.1.0)
│   ├── firmware_tool.py     # Main GUI application
│   ├── hexview.py           # Virtualized hex editor widget
│   ├── flasher.py           # Serial protocols (RAW, XMODEM, XMODEM-1K)
│   ├── requirements.txt     # Python dependencies (pyserial)
│   ├── tests/               # pytest suite (protocols, hex editor logic)
│   └── README.md            # Python GUI documentation
├── c/                       # C99 Command-Line Tool (v0.1.0)
│   ├── src/                 # C source and header files
│   ├── tests/               # CLI smoke test (smoke_test.sh)
│   ├── Makefile             # Build script for GCC/Clang/MinGW
│   ├── CMakeLists.txt       # CMake build configuration
│   └── README.md            # C CLI documentation
└── README.md                # Project overview (this file)
```

## Quick Start

**Run the Python GUI:**
```powershell
cd python
pip install -r requirements.txt
python firmware_tool.py
```

**Build & run the C CLI:**
```sh
cd c
make
./fwtool --version
```

## Development & CI

Every push and pull request runs these GitHub Actions workflows:

| Workflow | What it checks |
|---|---|
| **CI** (`ci.yml`) | Builds `fwtool` with GCC, Clang, CMake and MSVC on Linux, macOS and Windows, runs the CLI smoke test, and runs the Python test suite on Linux and Windows |
| **Lint & Style** (`lint.yml`) | `clang-format` (C style), `cppcheck` (C static analysis), `ruff` (Python lint + format), `actionlint` + `shellcheck` (workflows and scripts) |
| **CodeQL** (`codeql.yml`) | Security and quality analysis for C, Python and the workflows themselves (also weekly) |
| **Claude Code Review** (`claude-code-review.yml`) | AI review of pull requests with inline comments – needs the [Claude GitHub App](https://github.com/apps/claude) and an `ANTHROPIC_API_KEY` repository secret; skipped otherwise |

Run the same checks locally before pushing:

```sh
pip install clang-format==23.1.5 ruff==0.17.5 pytest pyserial
clang-format -i c/src/*.c c/src/*.h      # format C (style: .clang-format)
ruff check python && ruff format python  # lint + format Python (config: ruff.toml)
python -m pytest python/tests            # Python tests
cd c && make && bash tests/smoke_test.sh ./fwtool
```
