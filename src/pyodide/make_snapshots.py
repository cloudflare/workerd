import shutil
import sys
from pathlib import Path
from tempfile import TemporaryDirectory
from textwrap import indent

from tool_utils import hexdigest, run, timing

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
  compatibilityDate = "2023-12-18",
  compatibilityFlags = ["python_workers", {compat_flags}],
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
    run(
        [
            "bazel",
            "run",
            "@workerd//src/workerd/server:workerd",
            "--",
            "test",
            config_path,
            "--python-save-snapshot",
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


def main() -> int:
    compat_flags = ["python_workers_20250116"]
    with TemporaryDirectory() as package_cache:
        cache = Path(package_cache)
        cwd = Path.cwd()
        res = []
        with timing("baseline snapshot"):
            res += make_baseline_snapshot(cache, cwd, compat_flags)
        with timing("numpy snapshot"):
            res += make_numpy_snapshot(cache, cwd, compat_flags)
        with timing("fastapi snapshot"):
            res += make_fastapi_snapshot(cache, cwd, compat_flags)
    print()
    print("Update python_metadata.bzl:\n")
    for key, val in res:
        print(indent(f'"{key}": "{val}"', " " * 8))
    return 0


if __name__ == "__main__":
    sys.exit(main())
