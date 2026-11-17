"""The GUI must report the version from the repository's VERSION file."""

import os

import pytest

pytest.importorskip("tkinter")  # firmware_tool imports tkinter at module level

import firmware_tool

VERSION_FILE = os.path.join(os.path.dirname(__file__), "..", "..", "VERSION")


def test_version_matches_version_file():
    with open(VERSION_FILE, encoding="utf-8") as fh:
        expected = fh.read().strip()
    assert expected  # guards against an empty VERSION file
    assert firmware_tool.__version__ == expected
    assert firmware_tool.APP_TITLE.endswith(f"v{expected}")
