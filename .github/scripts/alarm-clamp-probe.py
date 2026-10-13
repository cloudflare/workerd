"""Exercise an expired deadline against the original and repaired fixtures."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

binary = Path("/tmp/alarm-runtime/workerd")
os.chmod(binary, 0o755)
source_path = "src/workerd/api/tests/actor-alarms-test.js"
original = subprocess.check_output(
    ["git", "show", "d77e96424:" + source_path], text=True
)
repaired = Path(source_path).read_text()
config = Path("src/workerd/api/tests/actor-alarms-test.wd-test").read_text()
results = []
for version, source, runs in [("original", original, 1), ("repaired", repaired, 5)]:
    assert source.count("Date.now() + 50") == 1
    source = source.replace("Date.now() + 50", "Date.now() - 50")
    for run in range(runs):
        with tempfile.TemporaryDirectory(prefix="alarm-clamp-") as directory:
            directory = Path(directory)
            (directory / "actor-alarms-test.js").write_text(source)
            path = directory / "actor-alarms-test.wd-test"
            path.write_text(config)
            storage = directory / "storage"
            storage.mkdir()
            result = subprocess.run(
                [str(binary), "test", str(path), "--experimental",
                 "--compat-date=2000-01-01",
                 "-dTEST_TMPDIR=" + str(storage)],
                text=True, capture_output=True, timeout=30,
            )
            output = result.stdout + result.stderr
            Path(f"alarm-{version}-{run + 1}.log").write_text(output)
            if version == "original":
                assert result.returncode != 0, output
                assert "Values are not strictly equal" in output, output
                assert "[ FAIL ]" in output, output
            else:
                assert result.returncode == 0, output
                assert "[ PASS ]" in output and "[ FAIL ]" not in output, output
            results.append(dict(version=version, run=run + 1,
                                exit_code=result.returncode))
Path("alarm-clamp-results.json").write_text(json.dumps(results, indent=2) + "\n")
print(json.dumps(results, indent=2))
