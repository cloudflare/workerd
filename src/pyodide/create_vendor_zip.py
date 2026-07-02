#!/usr/bin/env python3
"""
Script to vendor a Python package for vendored_py_wd_test.

This script uses uv to create a Pyodide-compatible virtual environment,
installs the specified packages from the Pyodide package index, and creates
a zip file of the resulting site-packages directory.

This is a partial/minimal copy of pywrangler.
Originally, this script used pywrangler directly, but we have a chicken and egg problem that
pywrangler should be updated after we support a new Python version in workerd, while this script
would often run when we are updating the Python version in workerd.
Hence we just maintain a partial copy of pywrangler here for installing packages through uv.
"""

import argparse
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path
from typing import Literal

from tool_utils import hexdigest

type PyVer = Literal["3.12", "3.13", "3.14"]

PYODIDE_INTERP_VERSIONS: dict[PyVer, str] = {
    "3.12": "3.12.7",
    "3.13": "3.13.2",
    "3.14": "3.14.2",
}

PYODIDE_INDEX_VERSIONS: dict[PyVer, str] = {
    "3.12": "0.27.7",
    "3.13": "0.28.3",
    "3.14": "314.0.0",
}


def get_interp_name(python: PyVer) -> str:
    v = PYODIDE_INTERP_VERSIONS[python]
    return f"cpython-{v}-emscripten-wasm32-musl"


def get_pyodide_index(python: PyVer) -> str:
    v = PYODIDE_INDEX_VERSIONS[python]
    return f"https://index.pyodide.org/{v}"


def run_uv(
    args: list[str | Path],
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    cmd = ["uv", *[str(a) for a in args]]
    print(f"  $ {' '.join(str(a) for a in cmd)}")
    res = subprocess.run(
        cmd, capture_output=True, text=True, check=False, cwd=cwd, env=env
    )
    if res.returncode:
        print(res.stdout)
        print(res.stderr)
        sys.exit(res.returncode)
    return res


def install_packages(package_names: list[str], python: PyVer, work_dir: Path) -> Path:
    venv_path = work_dir / "pyodide-venv"
    interp_name = get_interp_name(python)
    pyodide_index = get_pyodide_index(python)

    print(f"Creating Pyodide venv with {interp_name}...")
    run_uv(["venv", str(venv_path), "--python", interp_name])

    print("Installing packages...")
    env = os.environ.copy()
    env["VIRTUAL_ENV"] = str(venv_path)
    run_uv(
        [
            "pip",
            "install",
            "--extra-index-url",
            pyodide_index,
            "--index-strategy",
            "unsafe-best-match",
            "--no-build",
            *package_names,
        ],
        env=env,
    )

    major_minor = python
    site_packages = venv_path / "lib" / f"python{major_minor}" / "site-packages"
    if not site_packages.exists():
        print(f"Error: site-packages not found at {site_packages}")
        sys.exit(1)
    return site_packages


def create_zip_archive(source_dir: Path, output_dir: Path) -> bool:
    """Create a zip archive of the site-packages directory.

    Return value indicates whether the archive includes any binary modules (.so
    files).
    """
    tmp_path = output_dir / "tmp.zip"
    native = False

    with zipfile.ZipFile(tmp_path, "w", zipfile.ZIP_DEFLATED) as zipf:
        for file_path in source_dir.rglob("*"):
            if file_path.is_file():
                arcname = file_path.relative_to(source_dir)
                zipf.write(file_path, arcname)
                if file_path.suffix == ".so":
                    native = True

    return native


def vendor_package(package_names: list[str], python: PyVer) -> tuple[Path, bool]:
    tmp_dir = Path("/tmp")
    vendor_name = "-".join(package_names)
    work_dir = tmp_dir / f"vendor-{vendor_name}"

    if work_dir.exists():
        shutil.rmtree(work_dir)
    work_dir.mkdir(parents=True)

    try:
        print(f"Vendoring {package_names} for Python {python}...")
        site_packages = install_packages(package_names, python, work_dir)

        print("Creating zip archive...")
        native = create_zip_archive(site_packages, tmp_dir)
        py = f"-{python.replace('.', '')}" if native else ""
        zip_name = f"{vendor_name}{py}-vendored-for-ew-testing.zip"
        zip_path = tmp_dir / zip_name
        shutil.move(tmp_dir / "tmp.zip", zip_path)
    except Exception as e:
        print(f"Error vendoring packages {package_names!r}: {e}")
        sys.exit(1)
    else:
        print(f"Successfully created: {zip_path}")
        print(
            "Upload this zip file to the ew-snapshot-tests R2 bucket: "
            + "https://dash.cloudflare.com/e415f1017791ced9d5f3eb0df2b31c9e/r2/default/buckets/ew-snapshot-tests"
        )
        return zip_path, native
    finally:
        if work_dir.exists():
            shutil.rmtree(work_dir)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Create a zip file of a vendored Python package's source files for vendored_py_wd_test."
    )
    parser.add_argument(
        "package_name", help="Name of the Python package to vendor", nargs="*"
    )
    parser.add_argument(
        "-p", "--python", help="Name of the Python version to use", default="3.14"
    )

    args = parser.parse_args()

    if not args.package_name:
        print("Error: Package name is required")
        sys.exit(1)

    zip_path, native = vendor_package(args.package_name, args.python)
    print("Update python_metadata.bzl with:\n")
    abi = args.python if native else None
    i1 = " " * 12
    i2 = " " * 16
    print(i1 + "{")
    print(i2 + f'"name": "{"-".join(args.package_name)}",')
    print(i2 + f'"abi": {abi!r},')
    print(i2 + f'"sha256": "{hexdigest(zip_path)}",')
    print(i1 + "},")
    print()


if __name__ == "__main__":
    main()
