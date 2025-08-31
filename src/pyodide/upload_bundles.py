import argparse
import json
import re
import subprocess
import sys
from copy import deepcopy
from dataclasses import dataclass
from functools import cache
from os import environ
from pathlib import Path

import requests
from tool_utils import b64digest


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


def get_pyodide_bin_path(ver):
    return cquery(f"@workerd//src/pyodide:pyodide.capnp.bin@rule@{ver}")


def bundle_key(*, pyodide_version, pyodide_date, backport, **_kwds):
    return f"pyodide_{pyodide_version}_{pyodide_date}_{backport}.capnp.bin"


def bundle_url(**kwds):
    return "https://pyodide-capnp-bin.edgeworker.net/" + bundle_key(**kwds)


def get_backport(ver):
    info = bundle_version_info()[ver]
    backport = int(info["backport"])
    for b in range(backport + 1, backport + 20):
        info["backport"] = b
        url = bundle_url(**info)
        res = requests.head(url)
        if res.status_code == 404:
            return b
        res.raise_for_status()


def _get_replacer(backport, integrity):
    def replace_values(match):
        prefix = match.group(1)
        backport_key = match.group(2)
        middle = match.group(3)
        integrity_key = match.group(4)
        return (
            f'{prefix}{backport_key}"{backport}",{middle}{integrity_key}"{integrity}",'
        )

    return replace_values


@dataclass
class BundleInfo:
    version: str
    backport: int
    integrity: str
    path: Path


def update_python_metadata_bzl(bundles: list[BundleInfo]) -> None:
    """Update python_metadata.bzl file with new backport and integrity values."""
    metadata_path = (
        Path(__file__).parent.parent.parent / "build" / "python_metadata.bzl"
    )
    content = metadata_path.read_text()

    for info in bundles:
        # Find the version block and update backport and integrity
        version_pattern = rf'(\s+{{\s*\n\s*"name":\s*"{re.escape(info.version)}",.*?)("backport":\s*)"[^"]*",(.*?)("integrity":\s*)"[^"]*",'

        content = re.sub(
            version_pattern,
            _get_replacer(info.backport, info.integrity),
            content,
            flags=re.DOTALL,
        )

    metadata_path.write_text(content)


def print_info(info: BundleInfo) -> None:
    print(f"Uploading version {info.version} backport {info.backport}")
    i = " " * 8
    print("Update python_metadata.bzl with:\n")
    print(i + f'"backport": "{info.backport}",')
    print(i + f'"integrity": "{info.integrity}",')
    print()


def make_bundles(update_released: bool) -> list[BundleInfo]:
    result = []
    for ver, info in bundle_version_info().items():
        if ver.startswith("dev"):
            continue
        if not update_released and info.get("released", False):
            continue
        path = Path(get_pyodide_bin_path(ver)).resolve()
        b = get_backport(ver)
        info["backport"] = b
        integrity = b64digest(path)
        result.append(BundleInfo(ver, b, integrity, path))
    return result


def upload_bundles(bundles: list[BundleInfo]):
    from boto3 import client

    s3 = client(
        "s3",
        endpoint_url=f"https://{environ['R2_ACCOUNT_ID']}.r2.cloudflarestorage.com",
        aws_access_key_id=environ["R2_ACCESS_KEY_ID"],
        aws_secret_access_key=environ["R2_SECRET_ACCESS_KEY"],
        region_name="auto",
    )

    for bundle in bundles:
        ver = bundle.version
        path = Path(get_pyodide_bin_path(ver)).resolve()
        b = get_backport(ver)
        info = bundle_version_info()[ver]
        info["backport"] = b
        key = bundle_key(**info)
        s3.upload_file(str(path), "pyodide-capnp-bin", key)


def main():
    parser = argparse.ArgumentParser(
        description="Upload Pyodide bundles and update metadata"
    )
    parser.add_argument(
        "--update-released",
        action="store_true",
        help="Update already released versions?",
    )
    args = parser.parse_args()
    bundles = make_bundles(args.update_released)
    for bundle in bundles:
        print_info(bundle)
    update_python_metadata_bzl(bundles)
    upload_bundles(bundles)
    return 0


if __name__ == "__main__":
    sys.exit(main())
