# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""Checks that each config given enables the RUST_PYODIDE_BRIDGE autogate.

workerd ignores an autogate name that it doesn't know, and the Python tests that run with the gate
pass with the C++ classes too, so a misspelled or renamed gate would leave them passing without
the Rust resources. Arguments: the path of autogate.h, then the configs.
"""

import re
import sys

GATE = "rust-pyodide-bridge"


def known_gates(autogate_h):
    with open(autogate_h) as f:
        text = f.read()
    start = text.index("#define WORKERD_AUTOGATES(V)")
    end = text.index("// clang-format on", start)
    keys = re.findall(r"\bV\(([A-Z0-9_]+)\)", text[start:end])
    return {key.lower().replace("_", "-") for key in keys}


def main():
    known = known_gates(sys.argv[1])
    if GATE not in known:
        sys.exit(f"autogate.h declares no {GATE} autogate")
    for config in sys.argv[2:]:
        with open(config) as f:
            named = re.findall(r'"workerd-autogate-([a-z0-9-]+)"', f.read())
        unknown = [name for name in named if name not in known]
        if unknown:
            sys.exit(f"{config} names unknown autogates: {unknown}")
        if GATE not in named:
            sys.exit(f"{config} does not enable workerd-autogate-{GATE}")
    print(f"{len(sys.argv) - 2} configs enable workerd-autogate-{GATE}")


if __name__ == "__main__":
    main()
