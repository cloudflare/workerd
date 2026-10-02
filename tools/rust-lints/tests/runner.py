"""Run one Bazel-configured compiler invocation and compare its diagnostic output."""

import json
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    driver, expected_path, marker, *flags = sys.argv[1:]
    with tempfile.TemporaryDirectory() as out_dir:
        result = subprocess.run(
            [driver, *flags, "-o", str(Path(out_dir) / "fixture.rmeta")],
            text=True,
            capture_output=True,
        )
    if expected_path == "!failure":
        if result.returncode == 0:
            raise SystemExit(
                "Invalid Dylint library configuration unexpectedly succeeded"
            )
    else:
        lines = []
        for line in result.stderr.splitlines():
            try:
                diagnostic = json.loads(line)
            except json.JSONDecodeError:
                lines.append(line + "\n")
                continue
            code = (diagnostic.get("code") or {}).get("code")
            message = diagnostic.get("message", "")
            if diagnostic.get("level") != "error" or message.startswith(
                "aborting due to"
            ):
                continue
            primary = [span for span in diagnostic["spans"] if span["is_primary"]]
            filename = Path(primary[0]["file_name"]).name if primary else "<compiler>"
            label = f"error[{code}]" if code else "error"
            lines.append(f"{filename}: {label}: {message}\n")
        # Paths and pass traversal order are incidental; duplicates remain significant.
        actual = "".join(sorted(lines))
        expected = "" if expected_path == "-" else Path(expected_path).read_text()
        if actual != expected or (result.returncode != 0) != bool(expected):
            print(f"Expected ({expected_path}):\n{expected}", file=sys.stderr)
            print(f"Actual:\n{actual}", file=sys.stderr)
            print(result.stderr, file=sys.stderr)
            raise SystemExit(1)
    Path(marker).write_text("ok\n")


if __name__ == "__main__":
    main()
