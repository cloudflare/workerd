import argparse
import json
import re
import shutil
import subprocess
import sys
from copy import deepcopy
from functools import cache
from os import environ
from pathlib import Path
from tempfile import TemporaryDirectory
from textwrap import dedent, indent

from tool_utils import hexdigest, run, timing


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


TEMPLATE = """
using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    (name = "main", worker = .mainWorker),
  ],
);

const mainWorker :Workerd.Worker = (
  modules = [
    (name = "worker.py", pythonModule = embed "./worker.py"),
    {requirements}
  ],
  compatibilityDate = "2025-08-05",
  compatibilityFlags = ["python_no_global_handlers", {compat_flags}],
  # Learn more about compatibility dates at:
  # https://developers.cloudflare.com/workers/platform/compatibility-dates/
);
"""


def make_config(
    flags: list[str],
    reqs: list[str],
) -> str:
    requirements = ""
    for name in reqs:
        requirements += f'(name="{name}", pythonRequirement=""),'

    compat_flags = ""
    for flag in flags:
        compat_flags += f'"{flag}", '
    return TEMPLATE.format(requirements=requirements, compat_flags=compat_flags)


def make_worker(imports: list[str]) -> str:
    contents = ""
    for i in imports:
        contents += f"import {i}\n"
    contents += dedent("""\
    from workers import WorkerEntrypoint
    class Default(WorkerEntrypoint):
        def test(self):
            pass
    """)
    return contents


def make_snapshot(  # noqa: PLR0913
    d: Path,
    outdir: Path,
    outprefix: str,
    compat_flags: list[str],
    requirements: list[str],
    imports: list[str],
) -> str:
    config_path = d / "config.capnp"
    config_path.write_text(make_config(compat_flags, requirements))
    worker_path = d / "worker.py"
    worker_path.write_text(make_worker(imports))
    if imports:
        snapshot_flag = "--python-save-snapshot"
    else:
        snapshot_flag = "--python-save-baseline-snapshot"

    if "WORKERD_BINARY" in environ:
        workerd = [environ["WORKERD_BINARY"]]
    else:
        workerd = [
            "bazel",
            "run",
            "@workerd//src/workerd/server:workerd",
            "--",
        ]
    run(
        [
            *workerd,
            "test",
            config_path,
            snapshot_flag,
            "--pyodide-bundle-disk-cache-dir",
            d,
            "--pyodide-package-disk-cache-dir",
            d,
            "--experimental",
        ],
    )
    snapshot_path = d / "snapshot.bin"
    digest = hexdigest(snapshot_path)
    digest9 = digest[:9]
    outname = f"{outprefix}-{digest9}.bin"
    outfile = outdir / outname
    shutil.copyfile(snapshot_path, outfile)
    snapshot_path.unlink()
    return [outname, digest]


def make_baseline_snapshot(
    cache: Path, outdir: Path, compat_flags: list[str]
) -> list[tuple[str, str]]:
    name, digest = make_snapshot(cache, outdir, "baseline", compat_flags, [], [])
    return [
        ("baseline_snapshot", name),
        ("baseline_snapshot_hash", digest),
    ]


def make_numpy_snapshot(
    cache: Path, outdir: Path, compat_flags: list[str]
) -> list[tuple[str, str]]:
    name, digest = make_snapshot(
        cache, outdir, "package_snapshot_numpy", compat_flags, ["numpy"], ["numpy"]
    )
    return [
        ("numpy_snapshot", name),
        ("numpy_snapshot_hash", digest),
    ]


def make_fastapi_snapshot(
    cache: Path, outdir: Path, compat_flags: list[str]
) -> list[tuple[str, str]]:
    name, digest = make_snapshot(
        cache,
        outdir,
        "package_snapshot_fastapi",
        compat_flags,
        ["fastapi"],
        ["fastapi", "pydantic"],
    )
    return [
        ("fastapi_snapshot", name),
        ("fastapi_snapshot_hash", digest),
    ]


def make_snapshots(cache: Path, outdir: Path, update_released: bool):
    res = []
    for ver, info in bundle_version_info().items():
        if ver.startswith("dev"):
            continue
        if not update_released and info.get("released", False):
            continue
        compat_flags = list({"python_workers", info["enable_flag_name"]})

        ver_info = []
        with timing(f"version {ver} snapshots"):
            with timing("baseline snapshot"):
                ver_info += make_baseline_snapshot(cache, outdir, compat_flags)
            with timing("numpy snapshot"):
                ver_info += make_numpy_snapshot(cache, outdir, compat_flags)
            with timing("fastapi snapshot"):
                ver_info += make_fastapi_snapshot(cache, outdir, compat_flags)
        res.append((ver, ver_info))
    return res


def update_python_metadata_bzl(res):
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
        endpoint_url=f"https://{environ['PROD_R2_ACCOUNT_ID']}.r2.cloudflarestorage.com",
        aws_access_key_id=environ["PROD_R2_ACCESS_KEY_ID"],
        aws_secret_access_key=environ["PROD_R2_SECRET_ACCESS_KEY"],
        region_name="auto",
    )

    for file in outdir.glob("baseline-*.bin"):
        s3.upload_file(str(file), "pyodide-capnp-bin", file.name)

    s3 = client(
        "s3",
        endpoint_url=f"https://{environ['TEST_R2_ACCOUNT_ID']}.r2.cloudflarestorage.com",
        aws_access_key_id=environ["TEST_R2_ACCESS_KEY_ID"],
        aws_secret_access_key=environ["TEST_R2_SECRET_ACCESS_KEY"],
        region_name="auto",
    )
    for file in outdir.glob("*.bin"):
        s3.upload_file(str(file), "ew-snapshot-tests", file.name)


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
