"""Editing / undo logic of the HexView widget (needs Tk; skipped without a display)."""

import pytest

tk = pytest.importorskip("tkinter")

from hexview import HexView  # noqa: E402


@pytest.fixture
def view():
    root = None
    try:
        root = tk.Tk()
    except tk.TclError as e:  # no display available (headless Linux without xvfb)
        pytest.skip(f"Tk not available: {e}")
    root.withdraw()
    changes = []
    hv = HexView(root, on_change=lambda: changes.append(1))
    hv.load(bytes(64))
    changes.clear()
    hv.changes = changes
    yield hv
    root.destroy()


def test_write_bytes_is_one_undo_step_and_notifies_once(view):
    view.write_bytes(4, b"\xde\xad\x00\xef")  # byte 6 stays 0x00
    assert view.modified_count() == 3
    assert view.is_modified()
    assert len(view.changes) == 1

    view.undo()
    assert view.modified_count() == 0
    assert not view.is_modified()
    assert view.get_data() == bytes(64)
    assert view.cursor == 4


def test_undo_order(view):
    view.write_bytes(0, b"\x11\x22")
    view.set_byte(10, 0x55)
    view.undo()
    assert view.get_data()[10] == 0
    assert view.get_data()[:2] == b"\x11\x22"
    view.undo()
    assert view.get_data() == bytes(64)
    view.undo()  # nothing left: no error


def test_setting_original_value_clears_modified(view):
    view.set_byte(1, 7)
    assert view.modified_count() == 1
    view.set_byte(1, 0)
    assert view.modified_count() == 0
    assert not view.is_modified()


def test_mark_saved_resets_modified(view):
    view.set_byte(2, 9)
    view.mark_saved()
    assert not view.is_modified()
    assert view.original[2] == 9


def test_write_outside_file_raises(view):
    with pytest.raises(ValueError):
        view.write_bytes(62, b"\x01\x02\x03")
    with pytest.raises(ValueError):
        view.write_bytes(-1, b"\x01")
    assert not view.is_modified()
