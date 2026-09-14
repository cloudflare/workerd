import argparse
import json
import re
import shutil
import subprocess
import sys
import zipfile
from copy import deepcopy
from functools import cache
from os import environ
from pathlib import Path
from tempfile import TemporaryDirectory
from textwrap import dedent, indent
from urllib.request import Request, urlopen

from tool_utils import hexdigest, run, timing

# Buckets the snapshots and test packages live in. These match dep_pyodide.bzl.
PYODIDE_CAPN_BIN = "https://pyodide-capnp-bin.edgeworker.net/"
VENDOR_R2 = "https://pub-25a5b2f2f1b84655b185a505c7a3ad23.r2.dev/"

# Folders within the pyodide-capnp-bin bucket. Each snapshot is stored under its sha256 digest.
BASELINE_FOLDER = "baseline-snapshot"
DEDICATED_FOLDER = "dedicated-snapshot"

VENDOR_PKG_TESTS_DIR = (
    Path(__file__).parent.parent
    / "workerd"
    / "server"
    / "tests"
    / "python"
    / "vendor_pkg_tests"
)


def cquery(rule):
    res = subprocess.run(
        [
            "bazel",
            "cquery",
            rule,
            "--output=files",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if res.returncode:
        print(res.stdout)
        print(res.stderr)
        sys.exit(res.returncode)
    return res.stdout.strip()


@cache
def _bundle_version_info():
    with Path(cquery("@workerd//src/pyodide:bundle_version_info")).open() as f:
        return json.load(f)


def bundle_version_info():
    return deepcopy(_bundle_version_info())


def workerd_cmd() -> list[str]:
    if "WORKERD_BINARY" in environ:
        return [environ["WORKERD_BINARY"]]
    return [
        "bazel",
        "run",
        "@workerd//src/workerd/server:workerd",
        "--",
    ]


def download(url: str, dest: Path, sha256: str) -> Path:
    """Download url to dest unless it is already present, and verify its sha256."""
    if not dest.exists():
        # The buckets reject the default Python-urllib User-Agent with a 403.
        req = Request(url, headers={"User-Agent": "workerd-make-snapshots"})
        with urlopen(req) as response, dest.open("wb") as f:
            shutil.copyfileobj(response, f)
    digest = hexdigest(dest)
    if digest != sha256:
        print(f"Error: {url} has sha256 {digest}, expected {sha256}", file=sys.stderr)
        sys.exit(1)
    return dest


def store_snapshot(
    snapshot_path: Path, outdir: Path, folder: str, outprefix: str
) -> tuple[str, str, Path]:
    """Move snapshot_path into outdir/folder, named after its digest.

    Returns the file name, the sha256 hex digest, and the stored path.
    """
    digest = hexdigest(snapshot_path)
    digest9 = digest[:9]
    outname = f"{outprefix}-{digest9}.bin"
    outfile = outdir / folder / outname
    outfile.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(snapshot_path, outfile)
    return outname, digest, outfile


BASELINE_TEMPLATE = """
using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    (name = "main", worker = .mainWorker),
  ],
);

const mainWorker :Workerd.Worker = (
  modules = [
    (name = "worker.py", pythonModule = embed "./worker.py"),
  ],
  compatibilityDate = "2025-08-05",
  compatibilityFlags = ["python_no_global_handlers", {compat_flags}],
  # Learn more about compatibility dates at:
  # https://developers.cloudflare.com/workers/platform/compatibility-dates/
);
"""


def format_flags(flags: list[str]) -> str:
    return ", ".join(f'"{flag}"' for flag in flags)


def make_baseline_config(flags: list[str]) -> str:
    return BASELINE_TEMPLATE.format(compat_flags=format_flags(flags))


def make_baseline_worker() -> str:
    contents = dedent("""\
    from workers import WorkerEntrypoint
    class Default(WorkerEntrypoint):
        def test(self):
            pass
    """)
    return contents


def make_baseline_snapshot(
    d: Path, outdir: Path, compat_flags: list[str]
) -> tuple[Path, list[tuple[str, str]]]:
    """Create the baseline snapshot.

    Returns the path to the resulting snapshot file and the python_metadata.bzl entries.
    """
    config_path = d / "config.capnp"
    config_path.write_text(make_baseline_config(compat_flags))
    worker_path = d / "worker.py"
    worker_path.write_text(make_baseline_worker())

    run(
        [
            *workerd_cmd(),
            "test",
            config_path,
            "--python-save-baseline-snapshot",
            "--pyodide-bundle-disk-cache-dir",
            d,
            "--python-snapshot-dir",
            d,
            "--experimental",
        ],
    )
    name, digest, outfile = store_snapshot(
        d / "snapshot.bin", outdir, BASELINE_FOLDER, "baseline"
    )
    return outfile, [
        ("baseline_snapshot", name),
        ("baseline_snapshot_hash", digest),
    ]


def download_baseline_snapshot(d: Path, info: dict) -> Path:
    """Fetch the already-released baseline snapshot for a version."""
    digest = info["baseline_snapshot_hash"]
    return download(
        f"{PYODIDE_CAPN_BIN}{BASELINE_FOLDER}/{digest}",
        d / info["baseline_snapshot"],
        digest,
    )


def download_vendored_package(d: Path, pkg: dict, dest: Path) -> None:
    """Download and extract a `vendored_packages_for_tests` entry into dest.

    The URL scheme matches _py_vendor_test_deps in dep_pyodide.bzl.
    """
    abi = pkg["abi"]
    pyver = "-" + abi.replace(".", "") if abi else ""
    zip_name = f"{pkg['name']}{pyver}-vendored-for-ew-testing.zip"
    zip_path = download(VENDOR_R2 + zip_name, d / zip_name, pkg["sha256"])
    with zipfile.ZipFile(zip_path) as zf:
        zf.extractall(dest)


def make_vendored_module_list(config_dir: Path, modules_dir: Path) -> str:
    """Produce the `%PYTHON_VENDORED_MODULES%` substitution for a vendored package test.

    The output matches what vendor_pkg_tests/generate_modules.py produces for the Bazel test:
    every file is mounted under python_modules/ and embedded relative to the config file.
    """
    modules = []
    for path in sorted(modules_dir.rglob("*")):
        if path.is_dir():
            continue
        rel = path.relative_to(modules_dir).as_posix()
        embed_path = path.relative_to(config_dir).as_posix()
        kind = "pythonModule" if path.suffix == ".py" else "data"
        modules.append(
            f'(name = "python_modules/{rel}", {kind} = embed "{embed_path}")'
        )
    return ",\n".join(modules) + ",\n"


def make_numpy_vendor_snapshot(
    d: Path, outdir: Path, compat_flags: list[str], info: dict, baseline: Path
) -> list[tuple[str, str]]:
    """Create a dedicated snapshot of the numpy_vendor test, stacked on the baseline snapshot.

    This reproduces the snapshot-saving phase of the numpy_vendor_test Bazel target, so the
    resulting snapshot is what existing_dedicated_numpy_vendor_test loads.
    """
    work = d / f"numpy_vendor_{info['name']}"
    if work.exists():
        shutil.rmtree(work)
    work.mkdir()
    download_vendored_package(
        d, info["vendored_packages_for_tests"]["numpy"], work / "python_modules"
    )
    shutil.copyfile(VENDOR_PKG_TESTS_DIR / "numpy.py", work / "numpy.py")

    template = (VENDOR_PKG_TESTS_DIR / "numpy_vendor.wd-test").read_text()
    config = template.replace(
        "%PYTHON_VENDORED_MODULES%",
        make_vendored_module_list(work, work / "python_modules"),
    ).replace(
        "%PYTHON_FEATURE_FLAGS",
        format_flags([*compat_flags, "python_dedicated_snapshot"]),
    )
    config_path = work / "config.capnp"
    config_path.write_text(config)

    load_snapshot = d / "load_snapshot.bin"
    shutil.copyfile(baseline, load_snapshot)
    run(
        [
            *workerd_cmd(),
            "test",
            config_path,
            # numpy_vendor.wd-test leaves the compat date to the test runner.
            "--compat-date=2000-01-01",
            "--python-save-snapshot",
            "--python-load-snapshot",
            load_snapshot.name,
            "--pyodide-bundle-disk-cache-dir",
            d,
            "--python-snapshot-dir",
            d,
            "--experimental",
        ],
    )
    load_snapshot.unlink()
    name, digest, _ = store_snapshot(
        d / "snapshot.bin", outdir, DEDICATED_FOLDER, "dedicated-numpy-vendor"
    )
    return [
        ("dedicated_numpy_vendor_snapshot", name),
        ("dedicated_numpy_vendor_snapshot_hash", digest),
    ]


def supports_dedicated_snapshots(info: dict) -> bool:
    return info["pyodide_version"] != "0.26.0a2"


def make_snapshots(
    cache: Path, outdir: Path, update_released: bool
) -> list[tuple[str, list[tuple[str, str]]]]:
    res = []
    for ver, info in bundle_version_info().items():
        if ver.startswith("dev"):
            continue
        released = info.get("released", False)
        compat_flags = list({"python_workers", info["enable_flag_name"]})

        make_baseline = update_released or not released
        # Released versions keep their existing snapshots so that the stability tests keep
        # exercising the artifacts that were deployed, but a snapshot that has never been
        # generated still needs to be created.
        make_numpy_vendor = supports_dedicated_snapshots(info) and (
            make_baseline or not info.get("dedicated_numpy_vendor_snapshot")
        )
        if not make_baseline and not make_numpy_vendor:
            continue

        ver_info = []
        with timing(f"version {ver} snapshots"):
            baseline = None
            if make_baseline:
                with timing("baseline snapshot"):
                    baseline, kvs = make_baseline_snapshot(cache, outdir, compat_flags)
                    ver_info += kvs
            if make_numpy_vendor:
                with timing("dedicated numpy_vendor snapshot"):
                    if baseline is None:
                        baseline = download_baseline_snapshot(cache, info)
                    ver_info += make_numpy_vendor_snapshot(
                        cache, outdir, compat_flags, info, baseline
                    )
        res.append((ver, ver_info))
    return res


def update_python_metadata_bzl(res: list[tuple[str, list[tuple[str, str]]]]):
    """Update python_metadata.bzl file with new snapshot values."""
    metadata_path = (
        Path(__file__).parent.parent.parent / "build" / "python_metadata.bzl"
    )
    content = metadata_path.read_text()

    for ver, kvs in res:
        # Find the version block and update snapshot values
        version_pattern = rf'(\s+{{\s*\n\s*"name":\s*"{re.escape(ver)}",.*?)}}'

        def replace_version_block(match, *, kvs=kvs):
            block = match.group(1)
            # Update each key-value pair
            for key, val in kvs:
                key_pattern = rf'("{re.escape(key)}":\s*)"[^"]*"'
                block = re.sub(key_pattern, rf'\1"{val}"', block)
            return block + "}"

        content = re.sub(
            version_pattern, replace_version_block, content, flags=re.DOTALL
        )

    metadata_path.write_text(content)


def upload_snapshots(outdir: Path):
    from boto3 import client

    s3 = client(
        "s3",
        endpoint_url=f"https://{environ['R2_ACCOUNT_ID']}.r2.cloudflarestorage.com",
        aws_access_key_id=environ["R2_ACCESS_KEY_ID"],
        aws_secret_access_key=environ["R2_SECRET_ACCESS_KEY"],
        region_name="auto",
    )

    # outdir mirrors the bucket layout: <folder>/<file>, uploaded as <folder>/<sha256>.
    for file in outdir.glob("*/*.bin"):
        key = file.parent.name + "/" + hexdigest(file)
        s3.upload_file(str(file), "pyodide-capnp-bin", key)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Upload Pyodide bundles and update metadata"
    )
    parser.add_argument(
        "--update-released",
        action="store_true",
        help="Update already released versions?",
    )
    args = parser.parse_args()

    subprocess.run(
        [
            "bazel",
            "build",
            "@workerd//src/pyodide:bundle_version_info",
        ],
        check=True,
    )

    # Create generated-snapshots directory
    outdir = Path(__file__).parent / "generated-snapshots"
    if outdir.exists() and outdir.is_dir() and any(outdir.iterdir()):
        print(f"Error: Directory {outdir} exists and is not empty", file=sys.stderr)
        return 1
    outdir.mkdir(parents=True, exist_ok=True)

    with TemporaryDirectory() as package_cache:
        cache = Path(package_cache)
        res = make_snapshots(cache, outdir, args.update_released)

    update_python_metadata_bzl(res)

    upload_snapshots(outdir)
    print()
    print(
        "Upload these files to the ew-snapshot-tests R2 bucket: "
        + "https://dash.cloudflare.com/e415f1017791ced9d5f3eb0df2b31c9e/r2/default/buckets/ew-snapshot-tests"
    )
    print("Updated python_metadata.bzl with:")
    for ver, kvs in res:
        print("Version", ver)
        for key, val in kvs:
            print(indent(f'"{key}": "{val}",', " " * 8))
    return 0


if __name__ == "__main__":
    sys.exit(main())
