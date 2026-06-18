#!/usr/bin/env python3
"""Build-time tool: extract Python stdlib package wheels into a PythonPackages capnp message.

The CPython stdlib modules (and the shared libraries they depend on) used to be downloaded and
unpacked at request time. Instead we extract every file from each wheel here, at build time, and
embed them directly into the Pyodide bundle as a single PythonPackages message (schema in
src/pyodide/python_packages.capnp). The runtime mounts these files directly, with no gzip/tar work.

Each wheel's `install_dir` (from the pre-filtered lock file) determines where its files mount in the
worker filesystem ("site"/"stdlib" -> site-packages, "dynlib" -> /usr/lib).

Usage:
    pack_python_packages.py --capnp <capnp-tool> --lock <lock.json> --out <out.bin> <wheel>...
"""

import argparse
import json
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

# capnp text "string" / embed-filename escaping (paths are POSIX, but be safe).
def capnp_escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def extract(wheels: list[Path], lock: dict, work_dir: Path) -> list[tuple[str, str, Path]]:
    """Extract every regular file from each wheel listed in the lock file.

    Returns (install_dir, path, on_disk) tuples. Inputs not referenced by the lock file (e.g. the
    wheel repo's BUILD.bazel / REPO.bazel) are ignored.
    """
    by_name = {wheel.name: wheel for wheel in wheels}
    entries: list[tuple[str, str, Path]] = []
    files_dir = work_dir / "files"
    for index, pkg in enumerate(lock["packages"].values()):
        file_name = pkg["file_name"]
        install_dir = pkg["install_dir"]
        wheel = by_name.get(file_name)
        if wheel is None:
            raise SystemExit(f"Wheel {file_name} from lock file was not provided")
        with tarfile.open(wheel, "r:gz") as tar:
            for member in tar.getmembers():
                if member.isdir():
                    continue
                if not member.isfile():
                    raise SystemExit(
                        f"Unsupported tar entry type in {wheel.name}: {member.name}"
                    )
                path = member.name
                if path.startswith("./"):
                    path = path[2:]
                if not path:
                    continue
                on_disk = files_dir / str(index) / path
                on_disk.parent.mkdir(parents=True, exist_ok=True)
                src = tar.extractfile(member)
                assert src is not None
                on_disk.write_bytes(src.read())
                entries.append((install_dir, path, on_disk))
    return entries


def write_capnp(
    entries: list[tuple[str, str, Path]], work_dir: Path, schema_src: Path
) -> Path:
    # Copy the canonical schema into the work dir so the generated const file can `import` it.
    # Using the real schema (rather than re-declaring the structs here) keeps a single source of
    # truth: changes to python_packages.capnp can't silently diverge from what we serialize.
    (work_dir / "python_packages.capnp").write_text(schema_src.read_text())

    capnp_path = work_dir / "packages.capnp"
    lines = [
        "@0xf1b2c3d4e5a60798;",
        'using Schema = import "python_packages.capnp";',
        "",
        "const packages :Schema.PythonPackages = (files = [",
    ]
    for install_dir, path, on_disk in entries:
        embed = capnp_escape(str(on_disk.relative_to(work_dir)))
        lines.append(
            '  (installDir = "%s", path = "%s", contents = embed "%s"),'
            % (capnp_escape(install_dir), capnp_escape(path), embed)
        )
    lines.append("]);")
    capnp_path.write_text("\n".join(lines) + "\n")
    return capnp_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capnp", required=True, help="Path to the capnp tool")
    parser.add_argument("--schema", required=True, help="Path to python_packages.capnp")
    parser.add_argument("--lock", required=True, help="Path to the pre-filtered lock file")
    parser.add_argument("--out", required=True, help="Output path for the binary message")
    parser.add_argument("wheels", nargs="+", help="Wheel (.tar.gz) files to embed")
    args = parser.parse_args()

    # Resolve to absolute paths up front since capnp eval runs with cwd set to the work dir.
    capnp = str(Path(args.capnp).resolve())
    out_path = str(Path(args.out).resolve())
    schema_src = Path(args.schema).resolve()
    lock = json.loads(Path(args.lock).read_text())
    wheels = [Path(w) for w in args.wheels]

    with tempfile.TemporaryDirectory() as tmp:
        work_dir = Path(tmp)
        entries = extract(wheels, lock, work_dir)
        capnp_path = write_capnp(entries, work_dir, schema_src)
        with open(out_path, "wb") as out:
            subprocess.run(
                [capnp, "eval", capnp_path.name, "packages", "-o", "binary"],
                cwd=work_dir,
                stdout=out,
                check=True,
            )
    return 0


if __name__ == "__main__":
    sys.exit(main())
