# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""Dumps JSG RTTI with a workerd binary: workerd serves `workerd:rtti` (rtti-server.capnp) and
this script fetches one dump per spec.

Usage:
  python3 -I dump_rtti.py --workerd PATH --out DIR SPEC...

SPEC is a compatibility date optionally followed by `+flag` items, e.g. `2023-01-01` or
`2023-01-01+http_headers_getsetcookie`. Each dump is written to DIR/SPEC.capnp.bin.
"""

import argparse
import contextlib
import socket
import subprocess
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).absolute().parent


@contextlib.contextmanager
def rtti_server(workerd):
    """Runs workerd serving RTTI on a free port of the loopback interface and yields a function
    that fetches the dump for one spec."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        url = f"http://127.0.0.1:{listener.getsockname()[1]}/"
        proc = subprocess.Popen(
            [
                workerd,
                "serve",
                HERE / "rtti-server.capnp",
                "--experimental",
                "--socket-fd",
                f"http={listener.fileno()}",
            ],
            pass_fds=[listener.fileno()],
            stdout=subprocess.DEVNULL,
        )

        def fetch(spec, timeout=120):
            return urllib.request.urlopen(url + spec, timeout=timeout).read()

        try:
            # The kernel accepts connections as soon as the socket listens, so a request made
            # before workerd is up waits rather than failing. Probe with a short timeout until
            # workerd answers, and stop if it exits.
            deadline = time.monotonic() + 120
            while True:
                if proc.poll() is not None:
                    raise SystemExit(f"workerd exited with status {proc.returncode}")
                try:
                    fetch("2024-01-01", timeout=2)
                    break
                except OSError:
                    if time.monotonic() > deadline:
                        raise SystemExit("workerd did not start serving RTTI") from None
                    time.sleep(0.1)
            yield fetch
        finally:
            proc.terminate()
            proc.wait()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workerd", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("specs", nargs="+")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    with rtti_server(args.workerd) as get:
        for spec in args.specs:
            path = out / (spec + ".capnp.bin")
            path.write_bytes(get(spec))
            print(path, path.stat().st_size, "bytes")


if __name__ == "__main__":
    main()
