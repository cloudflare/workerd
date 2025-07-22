from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

CONFIG_TEMPLATE = """\
using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    (name = "main", worker = .mainWorker),
  ],
);

const mainWorker :Workerd.Worker = (
  modules = [
    {modules}
  ],
  compatibilityDate = "{date}",
  compatibilityFlags = [{flags}],
);
"""

PY_TEMPLATE = """\
{imports}

def test():
    pass
"""

def run(args):
    print(" ".join(str(x) for x in args))
    subprocess.run(args)


def make_config(outpath: Path, date: str, flags: list[str], packages: list[str], imports: list[str]):
    import_fmted = "\n".join("import " + i for i in imports)
    pyfile = PY_TEMPLATE.format(imports=import_fmted)
    # escape new lines
    pyfile = pyfile.replace("\n", "\\n")

    main_module = f'(name = "worker.py", pythonModule = "{pyfile}")'
    package_req = '(name = "{name}", pythonRequirement = "")'
    modules_list = [main_module, *(package_req.format(name=name) for name in packages)]
    modules = ",\n    ".join(modules_list)

    flags_str = ", ".join(f'"{flag}"' for flag in flags)
    outpath.write_text(CONFIG_TEMPLATE.format(pyfile=pyfile, modules=modules, date=date, flags=flags_str))

def make_snapshot(date: str, flags: list[str], packages: list[str], imports: list[str], baseline: bool):
    with TemporaryDirectory(delete=False) as d_str:
        d = Path(d_str)
        print(d)
        config_path = d / "config.capnp"
        make_config(config_path, date, flags, packages, imports)
        if baseline:
            snapshot_flag = "--python-save-baseline-snapshot"
        else:
            snapshot_flag = "--python-save-snapshot"

        run([
            "bazel",
            "run",
            "@workerd//src/workerd/server:workerd",
            "--",
            "test",
            config_path,
            snapshot_flag,
            "--pyodide-bundle-disk-cache-dir",
            d,
            "--pyodide-package-disk-cache-dir",
            d,
            "--experimental"
        ])


def make_package_snapshot(date: str, flags: list[str], packages: list[str], imports: list[str]):
    make_snapshot(date, flags, packages, imports, False)

def make_baseline_snapshot(date: str, flags: list[str]):
    make_snapshot(date, flags, [], [], True)


if __name__ == "__main__":
    date = "2023-12-18"
    flags = ["python_workers", "python_workers_20250116"]
    packages = ["numpy"]
    imports = "numpy"
    make_baseline_snapshot(date, flags)
    make_package_snapshot(date, flags, packages, imports)

