"""Prove the manager-retention regression covers Python module registration."""
from pathlib import Path
import re
import subprocess
import sys

if sys.argv[1] == "prepare":
    path = Path("src/workerd/io/worker-modules.h")
    source = path.read_text()
    old = "modules.addBuiltinBundle(pyodideBundle, holder);"
    assert source.count(old) == 1
    path.write_text(source.replace(old, "modules.addBuiltinBundle(pyodideBundle);"))
else:
    result = subprocess.run(
        ["bazel-bin/src/workerd/api/pyodide/wasm-module-test_binary"],
        text=True, capture_output=True, timeout=60,
    )
    output = result.stdout + result.stderr
    print(output)
    assert result.returncode != 0, output
    assert "observer.compiled == 1" in output, output
    assert "observer.fromCache == 1" in output, output
    assert re.search(r"\[ FAIL \].*Python module registration", output), output
    print("Negative control detected disconnected manager-owned Wasm.")
