"""Firmware Flash Tool – Binary laden, im Hex-Editor bearbeiten, seriell flashen."""

import os
import queue
import threading
import tkinter as tk
import zlib
from tkinter import filedialog, messagebox, ttk
from tkinter.scrolledtext import ScrolledText

from flasher import PARITIES, PROTOCOLS, STOPBITS, FlashCancelled, Flasher, list_ports
from hexview import HexView


def _read_version() -> str:
    """Reads the version from the repository's VERSION file (single source of truth)."""
    here = os.path.dirname(os.path.abspath(__file__))
    for path in (os.path.join(here, "VERSION"), os.path.join(here, "..", "VERSION")):
        try:
            with open(path, encoding="utf-8") as fh:
                return fh.read().strip()
        except OSError:
            continue
    return "unknown"


__version__ = _read_version()
APP_NAME = "Firmware Flash Tool"
APP_TITLE = f"{APP_NAME} v{__version__}"
BAUDRATES = ["9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600", "1000000", "2000000"]


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP_TITLE)
        self.geometry("1280x800")
        self.minsize(1050, 650)
        self._load_icon()

        self.filepath = None
        self.flasher = None
        self.worker = None
        self.msg_queue = queue.Queue()

        self._build_menu()
        self._build_ui()
        self._bind_shortcuts()
        self.refresh_ports()
        self._update_file_info()
        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self.after(50, self._poll_queue)

    def _load_icon(self):
        here = os.path.dirname(os.path.abspath(__file__))
        for assets in (os.path.join(here, "assets"), os.path.join(here, "..", "assets")):
            ico = os.path.join(assets, "icon.ico")
            png = os.path.join(assets, "icon.png")
            try:
                if os.name == "nt" and os.path.isfile(ico):
                    self.iconbitmap(ico)
                    return
                if os.path.isfile(png):
                    self._icon_img = tk.PhotoImage(file=png)
                    self.iconphoto(True, self._icon_img)
                    return
            except tk.TclError:
                pass

    # ================================================================== UI
    def _build_menu(self):
        m = tk.Menu(self)
        file_menu = tk.Menu(m, tearoff=False)
        file_menu.add_command(label="Öffnen ...", accelerator="Strg+O", command=self.open_file)
        file_menu.add_command(label="Speichern", accelerator="Strg+S", command=self.save_file)
        file_menu.add_command(label="Speichern unter ...", accelerator="Strg+Umschalt+S", command=self.save_file_as)
        file_menu.add_separator()
        file_menu.add_command(label="Beenden", command=self.on_close)
        m.add_cascade(label="Datei", menu=file_menu)

        edit_menu = tk.Menu(m, tearoff=False)
        edit_menu.add_command(label="Rückgängig", accelerator="Strg+Z", command=lambda: self.hexview.undo())
        edit_menu.add_command(label="Gehe zu Offset ...", accelerator="Strg+G", command=self._focus_goto)
        edit_menu.add_command(label="Alle Änderungen verwerfen", command=self.revert_changes)
        m.add_cascade(label="Bearbeiten", menu=edit_menu)
        self.config(menu=m)

    def _build_ui(self):
        style = ttk.Style(self)
        if "vista" in style.theme_names():
            style.theme_use("vista")
        style.configure("Accent.TButton", font=("Segoe UI", 10, "bold"))

        # Toolbar
        tb = ttk.Frame(self, padding=(6, 6, 6, 0))
        tb.pack(fill="x")
        ttk.Button(tb, text="📂 Firmware öffnen", command=self.open_file).pack(side="left")
        ttk.Button(tb, text="💾 Speichern", command=self.save_file).pack(side="left", padx=4)
        ttk.Button(tb, text="Speichern unter ...", command=self.save_file_as).pack(side="left")
        self.file_label = ttk.Label(tb, text="Keine Datei geladen", foreground="#666")
        self.file_label.pack(side="left", padx=12)

        # Hauptbereich: links Hex-Editor, rechts Seitenleiste
        paned = ttk.PanedWindow(self, orient="vertical")
        paned.pack(fill="both", expand=True, padx=6, pady=6)

        top = ttk.Frame(paned)
        paned.add(top, weight=4)

        self.hexview = HexView(top, on_change=self._update_file_info, on_cursor=self._update_cursor_info)
        self.hexview.pack(side="left", fill="both", expand=True)

        side = ttk.Frame(top, padding=(8, 0, 0, 0))
        side.pack(side="right", fill="y")
        self._build_info(side)
        self._build_patch(side)
        self._build_serial(side)

        # Log
        log_frame = ttk.LabelFrame(paned, text="Log", padding=4)
        paned.add(log_frame, weight=1)
        self.log_text = ScrolledText(log_frame, height=7, font=("Consolas", 9), state="disabled")
        self.log_text.pack(fill="both", expand=True)

        # Statusleiste
        self.status = ttk.Label(self, text="Bereit", relief="sunken", anchor="w", padding=(6, 2))
        self.status.pack(fill="x", side="bottom")

    def _build_info(self, parent):
        f = ttk.LabelFrame(parent, text="Datei", padding=6)
        f.pack(fill="x")
        self.info_vars = {}
        for row, key in enumerate(["Größe", "CRC32", "Summe (8 Bit)", "Geänderte Bytes"]):
            ttk.Label(f, text=key + ":").grid(row=row, column=0, sticky="w")
            v = tk.StringVar(value="-")
            ttk.Label(f, textvariable=v, font=("Consolas", 10)).grid(row=row, column=1, sticky="w", padx=6)
            self.info_vars[key] = v

        c = ttk.LabelFrame(parent, text="Cursor", padding=6)
        c.pack(fill="x", pady=(8, 0))
        self.cursor_vars = {}
        for row, key in enumerate(["Offset", "Hex", "Dez", "Bin", "UInt16 LE", "UInt32 LE"]):
            ttk.Label(c, text=key + ":").grid(row=row, column=0, sticky="w")
            v = tk.StringVar(value="-")
            ttk.Label(c, textvariable=v, font=("Consolas", 10)).grid(row=row, column=1, sticky="w", padx=6)
            self.cursor_vars[key] = v

    def _build_patch(self, parent):
        f = ttk.LabelFrame(parent, text="Bytes bearbeiten", padding=6)
        f.pack(fill="x", pady=(8, 0))
        ttk.Label(f, text="Offset (hex):").grid(row=0, column=0, sticky="w")
        self.goto_var = tk.StringVar()
        self.goto_entry = ttk.Entry(f, textvariable=self.goto_var, width=14, font=("Consolas", 10))
        self.goto_entry.grid(row=0, column=1, sticky="ew", padx=4)
        ttk.Button(f, text="Gehe zu", command=self.goto_offset).grid(row=0, column=2)
        self.goto_entry.bind("<Return>", lambda e: self.goto_offset())

        ttk.Label(f, text="Werte (hex):").grid(row=1, column=0, sticky="w", pady=(4, 0))
        self.patch_var = tk.StringVar()
        pe = ttk.Entry(f, textvariable=self.patch_var, width=14, font=("Consolas", 10))
        pe.grid(row=1, column=1, sticky="ew", padx=4, pady=(4, 0))
        ttk.Button(f, text="Schreiben", command=self.patch_bytes).grid(row=1, column=2, pady=(4, 0))
        pe.bind("<Return>", lambda e: self.patch_bytes())
        ttk.Label(
            f,
            text="z.B. DE AD BE EF – oder direkt im Editor tippen\n(Tab wechselt Hex/ASCII, Strg+Z = Undo)",
            foreground="#666",
            font=("Segoe UI", 8),
        ).grid(row=2, column=0, columnspan=3, sticky="w", pady=(4, 0))
        f.columnconfigure(1, weight=1)

    def _build_serial(self, parent):
        f = ttk.LabelFrame(parent, text="Serielle Schnittstelle / Flashen", padding=6)
        f.pack(fill="x", pady=(8, 0))
        r = 0
        ttk.Label(f, text="Port:").grid(row=r, column=0, sticky="w")
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(f, textvariable=self.port_var, width=24)
        self.port_combo.grid(row=r, column=1, sticky="ew", padx=4)
        ttk.Button(f, text="⟳", width=3, command=self.refresh_ports).grid(row=r, column=2)

        r += 1
        ttk.Label(f, text="Baudrate:").grid(row=r, column=0, sticky="w", pady=2)
        self.baud_var = tk.StringVar(value="115200")
        ttk.Combobox(f, textvariable=self.baud_var, values=BAUDRATES, width=12).grid(
            row=r, column=1, sticky="ew", padx=4, pady=2
        )

        r += 1
        ttk.Label(f, text="Parität:").grid(row=r, column=0, sticky="w", pady=2)
        self.parity_var = tk.StringVar(value=next(iter(PARITIES)))
        ttk.Combobox(f, textvariable=self.parity_var, values=list(PARITIES), state="readonly", width=12).grid(
            row=r, column=1, sticky="ew", padx=4, pady=2
        )

        r += 1
        ttk.Label(f, text="Stoppbits:").grid(row=r, column=0, sticky="w", pady=2)
        self.stop_var = tk.StringVar(value="1")
        ttk.Combobox(f, textvariable=self.stop_var, values=list(STOPBITS), state="readonly", width=12).grid(
            row=r, column=1, sticky="ew", padx=4, pady=2
        )

        r += 1
        ttk.Label(f, text="Protokoll:").grid(row=r, column=0, sticky="w", pady=2)
        self.proto_var = tk.StringVar(value=PROTOCOLS[0])
        proto = ttk.Combobox(f, textvariable=self.proto_var, values=PROTOCOLS, state="readonly", width=12)
        proto.grid(row=r, column=1, sticky="ew", padx=4, pady=2)
        proto.bind("<<ComboboxSelected>>", lambda e: self._update_proto_state())

        r += 1
        ttk.Label(f, text="Blockgröße (B):").grid(row=r, column=0, sticky="w", pady=2)
        self.chunk_var = tk.StringVar(value="256")
        self.chunk_entry = ttk.Entry(f, textvariable=self.chunk_var, width=12)
        self.chunk_entry.grid(row=r, column=1, sticky="ew", padx=4, pady=2)

        r += 1
        ttk.Label(f, text="Pause/Block (ms):").grid(row=r, column=0, sticky="w", pady=2)
        self.delay_var = tk.StringVar(value="0")
        self.delay_entry = ttk.Entry(f, textvariable=self.delay_var, width=12)
        self.delay_entry.grid(row=r, column=1, sticky="ew", padx=4, pady=2)

        r += 1
        ttk.Label(f, text="Startverzög. (ms):").grid(row=r, column=0, sticky="w", pady=2)
        self.start_delay_var = tk.StringVar(value="0")
        ttk.Entry(f, textvariable=self.start_delay_var, width=12).grid(row=r, column=1, sticky="ew", padx=4, pady=2)

        r += 1
        self.dtr_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(f, text="Reset-Impuls über DTR/RTS vor Start", variable=self.dtr_var).grid(
            row=r, column=0, columnspan=3, sticky="w", pady=2
        )

        r += 1
        bf = ttk.Frame(f)
        bf.grid(row=r, column=0, columnspan=3, sticky="ew", pady=(6, 2))
        self.flash_btn = ttk.Button(bf, text="⚡ Firmware flashen", style="Accent.TButton", command=self.start_flash)
        self.flash_btn.pack(side="left", fill="x", expand=True)
        self.cancel_btn = ttk.Button(bf, text="Abbrechen", command=self.cancel_flash, state="disabled")
        self.cancel_btn.pack(side="left", padx=(4, 0))

        r += 1
        self.progress = ttk.Progressbar(f, mode="determinate", maximum=100)
        self.progress.grid(row=r, column=0, columnspan=3, sticky="ew", pady=2)
        r += 1
        self.progress_label = ttk.Label(f, text="")
        self.progress_label.grid(row=r, column=0, columnspan=3, sticky="w")
        f.columnconfigure(1, weight=1)
        self._update_proto_state()

    def _bind_shortcuts(self):
        self.bind_all("<Control-o>", lambda e: self.open_file())
        self.bind_all("<Control-s>", lambda e: self.save_file())
        self.bind_all("<Control-S>", lambda e: self.save_file_as())
        self.bind_all("<Control-g>", lambda e: self._focus_goto())

    def _update_proto_state(self):
        raw = self.proto_var.get() == "RAW"
        state = "normal" if raw else "disabled"
        self.chunk_entry.config(state=state)
        self.delay_entry.config(state=state)

    # ============================================================ Datei
    def open_file(self):
        if not self._confirm_discard():
            return
        path = filedialog.askopenfilename(
            title="Firmware öffnen", filetypes=[("Firmware-Binary", "*.bin *.img *.fw *.rom"), ("Alle Dateien", "*.*")]
        )
        if not path:
            return
        try:
            with open(path, "rb") as fh:
                data = fh.read()
        except OSError as e:
            messagebox.showerror(APP_NAME, f"Datei konnte nicht gelesen werden:\n{e}")
            return
        self.filepath = path
        self.hexview.load(data)
        self.hexview.canvas.focus_set()
        self.log(f"Geladen: {path} ({len(data)} Bytes)")

    def save_file(self):
        if not self.filepath:
            return self.save_file_as()
        self._write(self.filepath)

    def save_file_as(self):
        if not self.hexview.data:
            return
        path = filedialog.asksaveasfilename(
            title="Firmware speichern unter",
            defaultextension=".bin",
            initialfile=os.path.basename(self.filepath) if self.filepath else "firmware.bin",
            filetypes=[("Firmware-Binary", "*.bin"), ("Alle Dateien", "*.*")],
        )
        if path:
            self._write(path)

    def _write(self, path):
        try:
            with open(path, "wb") as fh:
                fh.write(self.hexview.get_data())
        except OSError as e:
            messagebox.showerror(APP_NAME, f"Speichern fehlgeschlagen:\n{e}")
            return
        self.filepath = path
        self.hexview.mark_saved()
        self.log(f"Gespeichert: {path}")

    def revert_changes(self):
        if self.hexview.is_modified() and messagebox.askyesno(APP_NAME, "Alle ungespeicherten Änderungen verwerfen?"):
            self.hexview.load(self.hexview.original)

    def _confirm_discard(self):
        if not self.hexview.is_modified():
            return True
        res = messagebox.askyesnocancel(APP_NAME, "Es gibt ungespeicherte Änderungen. Jetzt speichern?")
        if res is None:
            return False
        if res:
            self.save_file()
            return not self.hexview.is_modified()
        return True

    # ======================================================= Bearbeiten
    def _focus_goto(self):
        self.goto_entry.focus_set()
        self.goto_entry.select_range(0, "end")

    @staticmethod
    def _parse_int(text):
        text = text.strip().lower()
        if text.startswith("0x"):
            text = text[2:]
        return int(text, 16)

    def goto_offset(self):
        try:
            off = self._parse_int(self.goto_var.get())
        except ValueError:
            messagebox.showwarning(APP_NAME, "Ungültiger Offset (hex erwartet, z.B. 1A0 oder 0x1A0)")
            return
        if not 0 <= off < len(self.hexview.data):
            messagebox.showwarning(APP_NAME, "Offset liegt außerhalb der Datei")
            return
        self.hexview.goto(off)

    def patch_bytes(self):
        if not self.hexview.data:
            return
        try:
            off = self._parse_int(self.goto_var.get()) if self.goto_var.get().strip() else self.hexview.cursor
            raw = self.patch_var.get().lower().replace("0x", "").replace(",", " ").replace(" ", "")
            values = bytes.fromhex(raw)
            if not values:
                raise ValueError("Keine Werte angegeben")
            self.hexview.write_bytes(off, values)
        except ValueError as e:
            messagebox.showwarning(APP_NAME, f"Eingabe ungültig:\n{e}")
            return
        self.log(f"{len(values)} Byte(s) ab 0x{off:08X} geschrieben")

    # ======================================================= Info-Panels
    def _update_file_info(self):
        data = self.hexview.data
        name = os.path.basename(self.filepath) if self.filepath else "Keine Datei geladen"
        mod = self.hexview.is_modified()
        self.file_label.config(text=name + (" *" if mod else ""))
        self.title(f"{APP_TITLE} – {name}{' *' if mod else ''}")
        if not data:
            for v in self.info_vars.values():
                v.set("-")
            return
        self.info_vars["Größe"].set(f"{len(data)} B (0x{len(data):X})")
        self.info_vars["CRC32"].set(f"0x{zlib.crc32(data) & 0xFFFFFFFF:08X}")
        self.info_vars["Summe (8 Bit)"].set(f"0x{sum(data) & 0xFF:02X}")
        self.info_vars["Geänderte Bytes"].set(str(self.hexview.modified_count()))

    def _update_cursor_info(self, idx):
        data = self.hexview.data
        if not data:
            for v in self.cursor_vars.values():  # keine veralteten Werte stehen lassen
                v.set("-")
            return
        b = data[idx]
        self.cursor_vars["Offset"].set(f"0x{idx:08X} ({idx})")
        self.cursor_vars["Hex"].set(f"0x{b:02X}")
        self.cursor_vars["Dez"].set(str(b))
        self.cursor_vars["Bin"].set(f"{b:08b}")
        u16 = data[idx : idx + 2]
        u32 = data[idx : idx + 4]
        self.cursor_vars["UInt16 LE"].set(str(int.from_bytes(u16, "little")) if len(u16) == 2 else "-")
        self.cursor_vars["UInt32 LE"].set(f"0x{int.from_bytes(u32, 'little'):08X}" if len(u32) == 4 else "-")
        self.status.config(text=f"Offset 0x{idx:08X}  |  Bereich: {self.hexview.area.upper()}")

    # ============================================================ Serial
    def refresh_ports(self):
        ports = list_ports()
        values = [f"{dev} – {desc}" for dev, desc in ports]
        self.port_combo["values"] = values
        # Exakter Vergleich des Gerätenamens – ein Präfixvergleich würde z.B. "COM1" als
        # noch vorhanden ansehen, wenn nur "COM10" existiert.
        current = self._selected_port()
        if values and current not in {dev for dev, _ in ports}:
            self.port_var.set(values[0])
        if not values:
            self.port_var.set("")
        self.log(f"{len(ports)} serielle(r) Port(s) gefunden")

    def _selected_port(self):
        return self.port_var.get().split(" – ")[0].strip()

    def start_flash(self):
        data = self.hexview.get_data()
        if not data:
            messagebox.showwarning(APP_NAME, "Bitte zuerst eine Firmware-Datei laden.")
            return
        port = self._selected_port()
        if not port:
            messagebox.showwarning(APP_NAME, "Bitte eine serielle Schnittstelle auswählen.")
            return
        try:
            baud = int(self.baud_var.get())
            chunk = int(self.chunk_var.get())
            delay = int(self.delay_var.get())
            start_delay = int(self.start_delay_var.get())
        except ValueError:
            messagebox.showwarning(APP_NAME, "Baudrate, Blockgröße und Pausen müssen Ganzzahlen sein.")
            return

        extra = ""
        if self.hexview.is_modified():
            extra = (
                f"\n\nHinweis: {self.hexview.modified_count()} Byte(s) wurden geändert (noch nicht gespeichert) – "
                "es wird der aktuelle Editor-Inhalt übertragen."
            )
        if not messagebox.askyesno(
            APP_NAME, f"{len(data)} Bytes an {port} @ {baud} Baud ({self.proto_var.get()}) übertragen?{extra}"
        ):
            return

        self.flasher = Flasher(
            port=port,
            baudrate=baud,
            protocol=self.proto_var.get(),
            parity=PARITIES[self.parity_var.get()],
            stopbits=STOPBITS[self.stop_var.get()],
            chunk_size=chunk,
            chunk_delay_ms=delay,
            dtr_reset=self.dtr_var.get(),
            start_delay_ms=start_delay,
            progress=lambda d, t: self.msg_queue.put(("progress", d, t)),
            log=lambda m: self.msg_queue.put(("log", m)),
        )
        self.progress["value"] = 0
        self.progress_label.config(text="")
        self.flash_btn.config(state="disabled")
        self.cancel_btn.config(state="normal")
        self.worker = threading.Thread(target=self._flash_worker, args=(data,), daemon=True)
        self.worker.start()

    def _flash_worker(self, data):
        try:
            self.flasher.run(data)
            self.msg_queue.put(("done", True, "Übertragung erfolgreich abgeschlossen."))
        except FlashCancelled:
            self.msg_queue.put(("done", False, "Übertragung abgebrochen."))
        except Exception as e:  # alle Fehler im GUI anzeigen
            self.msg_queue.put(("done", False, f"Fehler: {e}"))

    def cancel_flash(self):
        if self.flasher:
            self.flasher.cancel()
            self.log("Abbruch angefordert ...")

    def _poll_queue(self):
        try:
            while True:
                msg = self.msg_queue.get_nowait()
                if msg[0] == "log":
                    self.log(msg[1])
                elif msg[0] == "progress":
                    done, total = msg[1], msg[2]
                    pct = done * 100 / total if total else 100
                    self.progress["value"] = pct
                    self.progress_label.config(text=f"{done} / {total} Bytes ({pct:.1f} %)")
                elif msg[0] == "done":
                    ok, text = msg[1], msg[2]
                    self.log(text)
                    self.flash_btn.config(state="normal")
                    self.cancel_btn.config(state="disabled")
                    self.flasher = None
                    (messagebox.showinfo if ok else messagebox.showerror)(APP_NAME, text)
        except queue.Empty:
            pass
        self.after(50, self._poll_queue)

    # ============================================================ Diverses
    def log(self, text):
        from datetime import datetime

        self.log_text.config(state="normal")
        self.log_text.insert("end", f"[{datetime.now():%H:%M:%S}] {text}\n")
        self.log_text.see("end")
        self.log_text.config(state="disabled")

    def on_close(self):
        if self.flasher and not messagebox.askyesno(APP_NAME, "Eine Übertragung läuft noch. Trotzdem beenden?"):
            return
        if not self._confirm_discard():
            return
        if self.flasher:
            self.flasher.cancel()
        self.destroy()


if __name__ == "__main__":
    App().mainloop()
