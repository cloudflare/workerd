from pathlib import Path
import os

def test():
    fh = open(__file__, "r")
    print(fh.fileno())
    print("This file is {} characters long".format(os.lseek(fh.fileno(), 0, os.SEEK_END)))
