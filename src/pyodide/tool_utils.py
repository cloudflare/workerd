import subprocess
import sys
from base64 import b64encode
from contextlib import contextmanager
from hashlib import file_digest
from pathlib import Path
from time import time
from typing import Any


def run(
    cmd: list[str | Path],
    cwd: Path | str | None = None,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[Any]:
    """Execute a command and exit on failure with error output."""
    res = subprocess.run(
        cmd, capture_output=True, text=True, check=False, cwd=cwd, env=env
    )
    if res.returncode:
        print("Invocation failed:")
        print(res.stdout)
        print(res.stderr)
        sys.exit(res.returncode)
    return res


def bytesdigest(p: Path | str) -> bytes:
    """Calculate SHA256 digest of a file as bytes."""
    return file_digest(Path(p).open("rb"), "sha256").digest()


def hexdigest(p: Path | str) -> str:
    """Calculate SHA256 digest of a file as hex string."""
    digest = bytesdigest(p)
    return "".join(hex(e)[2:].zfill(2) for e in digest)


def b64digest(p: Path | str):
    return "sha256-" + b64encode(bytesdigest(p)).decode()


@contextmanager
def timing(name: str):
    """Context manager to time operations and print elapsed time."""
    print(f"Making {name}...")
    start = time()
    yield
    elapsed = time() - start
    print(f"  {elapsed:.2f}s")
