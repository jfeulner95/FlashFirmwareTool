#!/usr/bin/env bash
# Smoke test for the fwtool CLI (no serial hardware needed).
# Usage: c/tests/smoke_test.sh path/to/fwtool[.exe]
set -u

FW="${1:?usage: $0 path/to/fwtool}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
export NO_COLOR=1
fails=0

pass() { echo "  ok   $1"; }
fail() { echo "  FAIL $1"; fails=$((fails + 1)); }

# expect_exit <name> <code> <cmd...>
expect_exit() {
    local name="$1" want="$2"
    shift 2
    "$@" >"$WORK/out" 2>&1
    local got=$?
    if [ "$got" -eq "$want" ]; then pass "$name"; else fail "$name (exit $got, want $want)"; cat "$WORK/out"; fi
}

# expect_out <name> <text> : last command's output must contain text
expect_out() {
    if grep -qF -- "$2" "$WORK/out"; then pass "$1"; else fail "$1 (missing '$2')"; cat "$WORK/out"; fi
}

# 64-byte test image with bytes 0x00..0x3F
# (the octal escape is built into the format string on purpose)
# shellcheck disable=SC2059
for i in $(seq 0 63); do printf "\\$(printf '%03o' "$i")"; done >"$WORK/t.bin"

echo "fwtool smoke test: $FW"

expect_exit "--version" 0 "$FW" --version
expect_out "--version prints version" "fwtool 0.1.0"

expect_exit "info" 0 "$FW" info "$WORK/t.bin"
expect_out "info: size" "64 bytes (0x40)"
expect_out "info: CRC32" "0x100ECE8C"
expect_out "info: Sum8" "0xE0"

expect_exit "dump" 0 "$FW" dump "$WORK/t.bin" 10 10
expect_out "dump shows row 0x10" "00000010  10 11 12 13"

expect_exit "dump with huge length" 0 "$FW" dump "$WORK/t.bin" 0 FFFFFFFFFFFFFFFF
expect_out "dump shows last row" "00000030  30 31 32 33"

expect_exit "patch to new file" 0 "$FW" patch "$WORK/t.bin" 0x10 DE AD -o "$WORK/p.bin"
expect_exit "info of patched file" 0 "$FW" info "$WORK/p.bin"
expect_out "patched CRC32" "0x2C5F0897"
expect_exit "original left untouched" 0 "$FW" info "$WORK/t.bin"
expect_out "original CRC32" "0x100ECE8C"

expect_exit "patch out of range fails" 1 "$FW" patch "$WORK/t.bin" 40 00 -o "$WORK/x.bin"
expect_exit "patch with invalid hex fails" 2 "$FW" patch "$WORK/t.bin" 0 ZZ

# Regression: an out-of-range --patch must abort before touching the port (exit 2, not 1)
expect_exit "flash --patch out of range aborts" 2 "$FW" flash "$WORK/t.bin" NO_SUCH_PORT --patch 100=AA -y
expect_out "flash --patch error message" "outside the file"

expect_exit "unknown command" 2 "$FW" frobnicate
expect_exit "ports" 0 "$FW" ports

# Interactive editor: write, undo shows full range, out-of-range fill, save.
# Paths typed at the prompt are relative: on Windows (Git Bash) a POSIX /tmp path
# passed via stdin is not translated for the native executable.
FW_ABS="$(cd "$(dirname "$FW")" && pwd)/$(basename "$FW")"
printf 'w 4 11 05 22\nu\nfill 30 FFFFFFFF 00\nw 0 AA\ns e.bin\nq\n' >"$WORK/cmds"
expect_exit "edit session" 0 sh -c "cd \"$WORK\" && \"$FW_ABS\" edit t.bin <cmds"
expect_out "edit: write" "Wrote 3 byte(s) at 0x00000004"
expect_out "edit: undo" "Undone: 2 byte(s)"
expect_out "edit: undo shows whole range" "00000000              04 05 06"
expect_out "edit: fill range check" "Range is outside the file"
expect_out "edit: save" "Saved 64 bytes to e.bin"
expect_exit "edit result readable" 0 "$FW" dump "$WORK/e.bin" 0 2
expect_out "edit result content" "00000000  AA 01"

echo
if [ "$fails" -ne 0 ]; then
    echo "$fails check(s) failed"
    exit 1
fi
echo "all checks passed"
