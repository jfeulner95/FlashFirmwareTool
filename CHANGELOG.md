# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project uses [Semantic Versioning](https://semver.org/).

## [0.2.0] - 2026-11-17

### Added
- Release downloads: prebuilt `fwtool` binaries for Windows, Linux and macOS,
  and the GUI as a standalone Windows `.exe` (no Python installation needed),
  each with SHA-256 checksums.
- MIT license.

### Fixed
- `fwtool flash --patch` with an offset outside the file flashed the **unpatched**
  image; it now aborts without touching the serial port.
- Windows: pressing Ctrl+C in `fwtool edit` could quit the editor and discard
  unsaved changes.
- XMODEM (GUI): the first block could be sent twice when the receiver had
  queued extra start requests.
- GUI: "Cancel" now stops a transfer within 100 ms (previously up to 10 s, or
  not at all during RAW block pauses).
- GUI: editing large firmware images no longer slows down with every keystroke.
- GUI: Ctrl+Z undoes a multi-byte write in one step instead of byte by byte.
- GUI: the selected serial port is kept correctly when refreshing (`COM1` was
  confused with `COM10`).
- `fwtool`: file handle leak when saving failed; `fill` with a huge length
  allocated memory before checking the range; undo showed the wrong range.
- Linux/macOS: serial ports with very long device names are no longer listed
  with a truncated (wrong) path.

## [0.1.0] - 2026-11-16

### Added
- Initial release: Python/Tkinter GUI and C command-line tool (`fwtool`) for
  inspecting, patching and flashing firmware binaries over a serial port
  (RAW, XMODEM, XMODEM-1K).

[0.2.0]: https://github.com/jfeulner95/FlashFirmwareTool/releases/tag/v0.2.0
[0.1.0]: https://github.com/jfeulner95/FlashFirmwareTool/commit/d558109 "0.1.0 was never tagged"
