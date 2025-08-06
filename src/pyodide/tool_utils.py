import hashlib
import subprocess
import sys
from contextlib import contextmanager
from pathlib import Path
from time import time


def run(cmd: list[str | Path], cwd: Path | str | None = None):
    """Execute a command and exit on failure with error output."""
    res = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        check=False,
        cwd=cwd,
    )
    if res.returncode:
        print("Invocation failed:")
        print(res.stdout)
        print(res.stderr)
        sys.exit(res.returncode)


def bytesdigest(p: Path | str) -> bytes:
    """Calculate SHA256 digest of a file as bytes."""
    sha256_hash = hashlib.sha256()
    with Path(p).open("rb") as f:
        for chunk in iter(lambda: f.read(4096), b""):
            sha256_hash.update(chunk)
    return sha256_hash.digest()


def hexdigest(p: Path | str) -> str:
    """Calculate SHA256 digest of a file as hex string."""
    digest = bytesdigest(p)
    return "".join(hex(e)[2:] for e in digest)


@contextmanager
def timing(name: str):
    """Context manager to time operations and print elapsed time."""
    print(f"Making {name}...")
    start = time()
    yield
    elapsed = time() - start
    print(f"  {elapsed:.2f}s")
