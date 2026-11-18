import os
from pathlib import Path


def test_seek_metadata_fs():
    fh = Path(__file__).open()
    print(fh.fileno())
    print(f"This file is {os.lseek(fh.fileno(), 0, os.SEEK_END)} characters long")
