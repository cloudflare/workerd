#!/usr/bin/env python3
"""
Script to vendor a Python package for vendored_py_wd_test.

This script creates a pyproject.toml with the specified package as a dependency,
runs `uv run pywrangler sync` to download and prepare the package, and then
creates a zip file of the resulting python_modules directory.
"""

import argparse
import shutil
import sys
import zipfile
from pathlib import Path

from tool_utils import run

PYPROJECT_TEMPLATE = """[project]
name = "vendor-test"
version = "0.1.0"
requires-python = ">=3.12"
dependencies = [
    "{package_name}"
]

[dependency-groups]
dev = ["workers-py"]
"""


def create_pyproject_toml(package_name: str, target_dir: Path) -> Path:
    """Create a pyproject.toml file with the specified package as a dependency."""
    pyproject_content = PYPROJECT_TEMPLATE.format(package_name=package_name)
    pyproject_path = target_dir / "pyproject.toml"
    pyproject_path.write_text(pyproject_content)
    # Also create a wrangler file as otherwise pywrangler won't run
    wrangler_path = target_dir / "wrangler.toml"
    wrangler_path.write_text("\n")
    return pyproject_path


def run_pywrangler_sync(work_dir: Path) -> Path:
    """Run `uv run pywrangler sync` in the specified directory."""
    run(["uv", "run", "pywrangler", "sync"], cwd=work_dir)
    python_modules_dir = work_dir / "python_modules"
    if not python_modules_dir.exists():
        print(f"Error: python_modules directory not found at {python_modules_dir}")
        sys.exit(1)
    return python_modules_dir


def create_zip_archive(source_dir: Path, package_name: str, output_dir: Path) -> Path:
    """Create a zip archive of the python_modules directory."""
    zip_filename = f"{package_name}-vendored-for-ew-testing.zip"
    zip_path = output_dir / zip_filename

    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zipf:
        for file_path in source_dir.rglob("*"):
            if file_path.is_file():
                # Store relative path from python_modules directory
                arcname = file_path.relative_to(source_dir)
                zipf.write(file_path, arcname)

    return zip_path


def vendor_package(package_name: str) -> Path:
    """Main function to vendor a Python package."""
    tmp_dir = Path("/tmp")
    work_dir = tmp_dir / f"vendor-{package_name}"

    # Clean up any existing work directory
    if work_dir.exists():
        shutil.rmtree(work_dir)
    work_dir.mkdir(parents=True)

    try:
        # Create pyproject.toml
        print(f"Creating pyproject.toml in {work_dir}")
        create_pyproject_toml(package_name, work_dir)

        # Run pywrangler sync
        print("Running uv run pywrangler sync...")
        python_modules_dir = run_pywrangler_sync(work_dir)

        # Create zip archive
        print("Creating zip archive...")
        zip_path = create_zip_archive(python_modules_dir, package_name, tmp_dir)
    except Exception as e:
        print(f"Error vendoring package {package_name}: {e}")
        sys.exit(1)
    else:
        print(f"Successfully created: {zip_path}")
        print(
            "Upload this zip file to the ew-snapshot-tests R2 bucket: "
            + "https://dash.cloudflare.com/e415f1017791ced9d5f3eb0df2b31c9e/r2/default/buckets/ew-snapshot-tests"
        )
        return zip_path
    finally:
        # Clean up work directory
        if work_dir.exists():
            shutil.rmtree(work_dir)


def main() -> int:
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description="Create a zip file of a vendored Python package's source files for vendored_py_wd_test."
    )
    parser.add_argument("package_name", help="Name of the Python package to vendor")

    args = parser.parse_args()

    if not args.package_name:
        print("Error: Package name is required")
        return 1

    try:
        zip_path = vendor_package(args.package_name)
        print(f"\nVendoring complete! Archive created at: {zip_path}")
    except KeyboardInterrupt:
        print("\nOperation cancelled by user")
        return 1
    else:
        return 0


if __name__ == "__main__":
    sys.exit(main())
