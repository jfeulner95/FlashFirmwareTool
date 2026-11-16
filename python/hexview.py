"""Virtualisierter Hex-Editor als Tkinter-Widget.

Es werden nur die sichtbaren Zeilen gezeichnet, dadurch bleiben auch
Firmware-Images mit mehreren MB flüssig bedienbar.
"""

import string
import tkinter as tk
from tkinter import ttk, font as tkfont

BYTES_PER_ROW = 16

COLORS = {
    "bg": "#1e1e1e",
    "header": "#569cd6",
    "offset": "#858585",
    "hex": "#d4d4d4",
    "ascii": "#ce9178",
    "modified": "#ff5555",
    "cursor_bg": "#264f78",
    "mirror_bg": "#3a3d41",
    "nibble": "#ffcc00",
    "line": "#444444",
}


class HexView(ttk.Frame):
    def __init__(self, master, on_change=None, on_cursor=None, **kw):
        super().__init__(master, **kw)
        self.on_change = on_change
        self.on_cursor = on_cursor

        self.data = bytearray()
        self.original = b""
        self.undo_stack = []     # Liste von Gruppen: [(idx, alter_wert), ...]
        self._modified = set()   # Indizes, die vom Original abweichen (O(1)-Zähler)
        self.cursor = 0
        self.nibble = 0          # 0 = oberes Nibble, 1 = unteres Nibble
        self.area = "hex"        # "hex" oder "ascii"
        self.top_row = 0

        self.font = tkfont.Font(family="Consolas", size=11)
        self.cw = self.font.measure("0")
        self.lh = self.font.metrics("linespace") + 3
        self.pad = 8
        self.header_h = self.lh + 6
        self.x_off = self.pad
        self.x_hex = self.pad + 10 * self.cw
        self.x_ascii = self.x_hex + (BYTES_PER_ROW * 3 + 3) * self.cw
        width = self.x_ascii + BYTES_PER_ROW * self.cw + self.pad

        self.canvas = tk.Canvas(
            self, bg=COLORS["bg"], width=width, height=500,
            highlightthickness=1, highlightbackground=COLORS["line"],
            highlightcolor=COLORS["header"], takefocus=True,
        )
        self.vbar = ttk.Scrollbar(self, orient="vertical", command=self._on_scrollbar)
        self.canvas.grid(row=0, column=0, sticky="nsew")
        self.vbar.grid(row=0, column=1, sticky="ns")
        self.rowconfigure(0, weight=1)
        self.columnconfigure(0, weight=1)

        c = self.canvas
        c.bind("<Configure>", lambda e: self.redraw())
        c.bind("<Button-1>", self._on_click)
        c.bind("<Key>", self._on_key)
        c.bind("<MouseWheel>", self._on_wheel)                       # Windows / macOS
        c.bind("<Button-4>", lambda e: self.scroll_rows(-3))         # Linux
        c.bind("<Button-5>", lambda e: self.scroll_rows(3))

    # ------------------------------------------------------------------ API
    def load(self, data: bytes):
        self.data = bytearray(data)
        self.original = bytes(data)
        self.undo_stack.clear()
        self._modified.clear()
        self.cursor = 0
        self.nibble = 0
        self.top_row = 0
        self.redraw()
        self._notify_change()
        self._notify_cursor()

    def get_data(self) -> bytes:
        return bytes(self.data)

    def mark_saved(self):
        self.original = bytes(self.data)
        self._modified.clear()
        self.redraw()
        self._notify_change()

    def modified_count(self) -> int:
        return len(self._modified)

    def is_modified(self) -> bool:
        return bool(self._modified)

    def _store(self, idx: int, value: int):
        self.data[idx] = value
        if value != self.original[idx]:
            self._modified.add(idx)
        else:
            self._modified.discard(idx)

    def _apply(self, idx: int, value: int, group: list) -> None:
        value &= 0xFF
        old = self.data[idx]
        if old != value:
            group.append((idx, old))
            self._store(idx, value)

    def set_byte(self, idx: int, value: int):
        if not 0 <= idx < len(self.data):
            return
        group = []
        self._apply(idx, value, group)
        if group:
            self.undo_stack.append(group)
            self._notify_change()

    def write_bytes(self, offset: int, values: bytes):
        """Schreibt mehrere Bytes ab offset (überschreibend, keine Größenänderung).

        Alle Bytes bilden einen Undo-Schritt; on_change wird nur einmal ausgelöst.
        """
        if offset < 0 or offset + len(values) > len(self.data):
            raise ValueError("Bereich liegt außerhalb der Datei")
        group = []
        for i, v in enumerate(values):
            self._apply(offset + i, v, group)
        if group:
            self.undo_stack.append(group)
            self._notify_change()
        self.goto(offset)

    def undo(self):
        if not self.undo_stack:
            return
        group = self.undo_stack.pop()
        for idx, old in reversed(group):
            self._store(idx, old)
        self.cursor = min(idx for idx, _ in group)
        self.nibble = 0
        self._ensure_visible()
        self.redraw()
        self._notify_change()
        self._notify_cursor()

    def goto(self, offset: int):
        if not self.data:
            return
        self.cursor = max(0, min(offset, len(self.data) - 1))
        self.nibble = 0
        self._ensure_visible(center=True)
        self.redraw()
        self._notify_cursor()
        self.canvas.focus_set()

    # -------------------------------------------------------------- Geometrie
    def _hex_x(self, i):
        return self.x_hex + (i * 3 + (1 if i >= 8 else 0)) * self.cw

    def _ascii_x(self, i):
        return self.x_ascii + i * self.cw

    def _total_rows(self):
        return (len(self.data) + BYTES_PER_ROW - 1) // BYTES_PER_ROW

    def _visible_rows(self):
        h = self.canvas.winfo_height() - self.header_h
        return max(1, h // self.lh)

    def _clamp_top(self):
        max_top = max(0, self._total_rows() - self._visible_rows())
        self.top_row = max(0, min(self.top_row, max_top))

    def _ensure_visible(self, center=False):
        row = self.cursor // BYTES_PER_ROW
        vis = self._visible_rows()
        if center and not (self.top_row <= row < self.top_row + vis):
            self.top_row = row - vis // 2
        elif row < self.top_row:
            self.top_row = row
        elif row >= self.top_row + vis:
            self.top_row = row - vis + 1
        self._clamp_top()

    # ------------------------------------------------------------- Zeichnen
    def redraw(self):
        c = self.canvas
        c.delete("all")
        f = self.font
        self._clamp_top()

        # Kopfzeile
        c.create_text(self.x_off, 3, text="Offset", anchor="nw", font=f, fill=COLORS["header"])
        for i in range(BYTES_PER_ROW):
            c.create_text(self._hex_x(i), 3, text=f"{i:02X}", anchor="nw", font=f, fill=COLORS["header"])
            c.create_text(self._ascii_x(i), 3, text=f"{i:X}", anchor="nw", font=f, fill=COLORS["header"])
        c.create_line(0, self.header_h - 2, 10000, self.header_h - 2, fill=COLORS["line"])

        n = len(self.data)
        vis = self._visible_rows()
        for r in range(vis + 1):
            row = self.top_row + r
            start = row * BYTES_PER_ROW
            if start >= n:
                break
            y = self.header_h + r * self.lh
            c.create_text(self.x_off, y, text=f"{start:08X}", anchor="nw", font=f, fill=COLORS["offset"])
            for i in range(min(BYTES_PER_ROW, n - start)):
                idx = start + i
                b = self.data[idx]
                hx, ax = self._hex_x(i), self._ascii_x(i)
                if idx == self.cursor:
                    hex_bg = COLORS["cursor_bg"] if self.area == "hex" else COLORS["mirror_bg"]
                    asc_bg = COLORS["cursor_bg"] if self.area == "ascii" else COLORS["mirror_bg"]
                    c.create_rectangle(hx - 2, y, hx + 2 * self.cw + 1, y + self.lh - 1, fill=hex_bg, outline="")
                    c.create_rectangle(ax, y, ax + self.cw, y + self.lh - 1, fill=asc_bg, outline="")
                    if self.area == "hex":
                        nx = hx + self.nibble * self.cw
                        c.create_line(nx, y + self.lh - 2, nx + self.cw, y + self.lh - 2,
                                      fill=COLORS["nibble"], width=2)
                modified = idx < len(self.original) and self.original[idx] != b
                c.create_text(hx, y, text=f"{b:02X}", anchor="nw", font=f,
                              fill=COLORS["modified"] if modified else COLORS["hex"])
                ch = chr(b) if 32 <= b < 127 else "."
                c.create_text(ax, y, text=ch, anchor="nw", font=f,
                              fill=COLORS["modified"] if modified else COLORS["ascii"])

        total = self._total_rows()
        if total:
            self.vbar.set(self.top_row / total, min(1.0, (self.top_row + vis) / total))
        else:
            self.vbar.set(0, 1)

    # ------------------------------------------------------------ Ereignisse
    def scroll_rows(self, n):
        self.top_row += n
        self._clamp_top()
        self.redraw()

    def _on_scrollbar(self, *args):
        if args[0] == "moveto":
            self.top_row = int(float(args[1]) * self._total_rows())
        elif args[0] == "scroll":
            n = int(args[1])
            if args[2] == "pages":
                n *= self._visible_rows()
            self.top_row += n
        self._clamp_top()
        self.redraw()

    def _on_wheel(self, event):
        self.scroll_rows(-3 if event.delta > 0 else 3)

    def _on_click(self, event):
        self.canvas.focus_set()
        if event.y < self.header_h or not self.data:
            return
        row = self.top_row + (event.y - self.header_h) // self.lh
        col, area, nib = None, None, 0
        for i in range(BYTES_PER_ROW):
            hx = self._hex_x(i)
            if hx - 2 <= event.x < hx + 2 * self.cw + 2:
                col, area = i, "hex"
                nib = 0 if event.x < hx + self.cw else 1
                break
            ax = self._ascii_x(i)
            if ax <= event.x < ax + self.cw:
                col, area = i, "ascii"
                break
        if col is None:
            return
        idx = row * BYTES_PER_ROW + col
        if idx >= len(self.data):
            return
        self.cursor, self.area, self.nibble = idx, area, nib
        self.redraw()
        self._notify_cursor()

    def _on_key(self, event):
        if not self.data:
            return "break"
        ks = event.keysym
        ctrl = bool(event.state & 0x4)
        n = len(self.data)
        vis = self._visible_rows()

        if ctrl:
            if ks.lower() == "z":
                self.undo()
                return "break"
            if ks == "Home":
                self._move_to(0)
                return "break"
            if ks == "End":
                self._move_to(n - 1)
                return "break"
            return None  # andere Shortcuts (Strg+O, Strg+S, ...) durchreichen

        moves = {
            "Left": -1, "Right": 1,
            "Up": -BYTES_PER_ROW, "Down": BYTES_PER_ROW,
            "Prior": -BYTES_PER_ROW * vis, "Next": BYTES_PER_ROW * vis,
        }
        if ks in moves:
            self._move_to(self.cursor + moves[ks])
            return "break"
        if ks == "Home":
            self._move_to(self.cursor - self.cursor % BYTES_PER_ROW)
            return "break"
        if ks == "End":
            self._move_to(self.cursor - self.cursor % BYTES_PER_ROW + BYTES_PER_ROW - 1)
            return "break"
        if ks == "Tab":
            self.area = "ascii" if self.area == "hex" else "hex"
            self.nibble = 0
            self.redraw()
            return "break"

        ch = event.char
        if not ch:
            return None
        if self.area == "hex" and ch in string.hexdigits:
            d = int(ch, 16)
            b = self.data[self.cursor]
            if self.nibble == 0:
                self.set_byte(self.cursor, (d << 4) | (b & 0x0F))
                self.nibble = 1
            else:
                self.set_byte(self.cursor, (b & 0xF0) | d)
                self.nibble = 0
                if self.cursor < n - 1:
                    self.cursor += 1
            self._ensure_visible()
            self.redraw()
            self._notify_cursor()
            return "break"
        if self.area == "ascii" and 32 <= ord(ch) < 127:
            self.set_byte(self.cursor, ord(ch))
            if self.cursor < n - 1:
                self.cursor += 1
            self._ensure_visible()
            self.redraw()
            self._notify_cursor()
            return "break"
        return None

    def _move_to(self, idx):
        self.cursor = max(0, min(idx, len(self.data) - 1))
        self.nibble = 0
        self._ensure_visible()
        self.redraw()
        self._notify_cursor()

    # ------------------------------------------------------------ Callbacks
    def _notify_change(self):
        if self.on_change:
            self.on_change()

    def _notify_cursor(self):
        if self.on_cursor:
            self.on_cursor(self.cursor)
