"""Check native argument forwarding through the generated wd-test batch file."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

wrapper = Path("bazel-bin/src/workerd/api/tests/persistent-stubs-test@_wd_test.bat").resolve()
assert wrapper.is_file(), wrapper
values = ["plain", "two words", "", "literal!bang", "semi;colon", "slash/path",
          "backslash\\path", "--long=some words"] + [f"argument-{n}" for n in range(12)]
with tempfile.TemporaryDirectory(prefix="argument probe ") as directory:
    directory = Path(directory)
    child = directory / "record arguments.py"
    output = directory / "arguments.json"
    child.write_text("import json,sys\nfrom pathlib import Path\n"
                     "Path(sys.argv[1]).write_text(json.dumps(sys.argv[2:]))\n")
    environment = dict(os.environ, TEST_TMPDIR=str(directory / "storage with spaces"))
    command = [str(wrapper), sys.executable, str(child), str(output)] + values
    result = subprocess.run(command, env=environment, text=True, capture_output=True, timeout=15)
    print(result.stdout + result.stderr)
    assert result.returncode == 0, result.returncode
    expected = values + ["-dTEST_TMPDIR=" + environment["TEST_TMPDIR"]]
    actual = json.loads(output.read_text())
    assert actual == expected, (actual, expected)
    Path("windows-arguments-results.json").write_text(json.dumps(actual, indent=2) + "\n")
    print("Native wrapper forwarded all arguments, including spaces, empty values and bangs.")
