"""Verify that the regression detects raw one-byte ESM cache generation."""
from pathlib import Path
import re
import subprocess
import sys

if sys.argv[1] == "prepare":
    path = Path("src/rust/gen-compile-cache/cxx-bridge.c++")
    source = path.read_text()
    source, count = re.subn(
        r"auto contentStr = asFunction \? js\.strExtern\(jsg::copyToArc\(kj::from<Rust>\(source\)\)\)"
        r"\s+: js\.str\(kj::from<Rust>\(source\)\);",
        "auto contentStr = js.strExtern(jsg::copyToArc(kj::from<Rust>(source)));",
        source,
    )
    assert count == 1, count
    path.write_text(source)
else:
    result = subprocess.run(
        ["bazel-bin/src/workerd/jsg/compile-cache-test_binary"],
        text=True, capture_output=True, timeout=60,
    )
    output = result.stdout + result.stderr
    print(output)
    assert result.returncode != 0, output
    rejected = [line for line in output.splitlines()
                if "source.GetCachedData()->rejected" in line]
    assert len(rejected) == 2, rejected
    assert any("latin1" in line for line in rejected), rejected
    assert any("utf16" in line for line in rejected), rejected
    print("Negative control rejected both non-ASCII caches.")
