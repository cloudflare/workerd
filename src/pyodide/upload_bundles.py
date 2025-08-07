import json
import subprocess
import sys
from copy import deepcopy
from functools import cache
from os import environ
from pathlib import Path

import requests
from boto3 import client
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
    for b in range(backport + 1, backport + 10):
        info["backport"] = b
        url = bundle_url(**info)
        res = requests.head(url)
        if res.status_code == 404:
            return b
        res.raise_for_status()


def main():
    s3 = client(
        "s3",
        endpoint_url=f"https://{environ['R2_ACCOUNT_ID']}.r2.cloudflarestorage.com",
        aws_access_key_id=environ["R2_ACCESS_KEY_ID"],
        aws_secret_access_key=environ["R2_SECRET_ACCESS_KEY"],
        region_name="auto",
    )

    for ver, info in bundle_version_info().items():
        if ver.startswith("dev"):
            continue
        path = Path(get_pyodide_bin_path(ver)).resolve()
        b = get_backport(ver)
        info["backport"] = b
        key = bundle_key(**info)
        print(f"Uploading version {ver} backport {b}")
        shasum = b64digest(path)
        i = " " * 8
        print("Update python_metadata.bzl with:\n")
        print(i + f'"backport": "{b}",')
        print(i + f'"integrity": "{shasum}",')
        print()
        s3.upload_file(str(path), "pyodide-capnp-bin", key)
    return 0


if __name__ == "__main__":
    sys.exit(main())
