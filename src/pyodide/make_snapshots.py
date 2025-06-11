import shutil
import subprocess
import sys
from base64 import b64encode
from hashlib import file_digest
from pathlib import Path
from tempfile import TemporaryDirectory

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
    flags,
    reqs,
):
    requirements = ""
    for name in reqs:
        requirements += f'(name="{name}", pythonRequirement=""),'

    compat_flags = ""
    for flag in flags:
        compat_flags += f'"{flag}", '
    return TEMPLATE.format(requirements=requirements, compat_flags=compat_flags)


def make_worker(imports):
    contents = ""
    for i in imports:
        contents += f"import {i}\n"
    return contents


def run(cmd):
    res = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        check=False,
    )
    if res.returncode:
        print("Invocation failed:")
        print(res.stdout)
        print(res.stderr)
        sys.exit(res.returncode)


def make_snapshot(  # noqa: PLR0913
    d: Path,
    outdir: Path,
    outprefix: str,
    compat_flags: list[str],
    requirements: list[str],
    imports: list[str],
):
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
    digest9 = hexdigest(snapshot_path)[:9]
    outname = f"{outprefix}-{digest9}.bin"
    outfile = outdir / outname
    shutil.copyfile(snapshot_path, outfile)
    snapshot_path.unlink()
    return outname


def hexdigest(p: Path | str) -> str:
    p1 = Path(p)
    digest = file_digest(p1.open("rb"), "sha256").digest()
    return "".join(hex(e)[2:] for e in digest)


def b64digest(p: Path) -> str:
    digest = file_digest(p.open("rb"), "sha256").digest()
    return "sha256-" + b64encode(digest).decode()


def make_baseline_snapshot(cache, outdir, compat_flags):
    name = make_snapshot(cache, outdir, "baseline", compat_flags, [], [])
    digest = hexdigest(outdir / name)
    print(
        f"""\
        "baseline_snapshot": "{name}",
        "baseline_snapshot_hash": "{digest}",\
        """
    )


def make_numpy_snapshot(cache, outdir, compat_flags):
    name = make_snapshot(
        cache, outdir, "package_snapshot_numpy", compat_flags, ["numpy"], ["numpy"]
    )
    digest = b64digest(outdir / name)
    print(
        f"""\
        "numpy_snapshot": "{name}",
        "numpy_snapshot_integrity": "{digest}",\
        """
    )


def make_fastapi_snapshot(cache, outdir, compat_flags):
    name = make_snapshot(
        cache,
        outdir,
        "package_snapshot_fastapi",
        compat_flags,
        ["fastapi"],
        ["fastapi", "pydantic"],
    )
    digest = b64digest(outdir / name)
    print(
        f"""\
        "fastapi_snapshot": "{name}",
        "fastapi_snapshot_integrity": "{digest}",\
        """
    )


def main():
    compat_flags = ["python_workers_20250116"]
    with TemporaryDirectory() as package_cache:
        cache = Path(package_cache)
        cwd = Path.cwd()
        make_baseline_snapshot(cache, cwd, compat_flags)
        make_numpy_snapshot(cache, cwd, compat_flags)
        make_fastapi_snapshot(cache, cwd, compat_flags)
    return 0


if __name__ == "__main__":
    sys.exit(main())
