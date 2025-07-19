# This script reads the list of files from a zip file and outputs a list of Cap'n Proto module
# definitions to be used in a .wd-test for Python tests.
import sys
from pathlib import Path

modules = []
for f_path in sys.argv[1:]:
    # The path from bazel is relative to the exec root, e.g.:
    # external/fastapi_src/fastapi/__init__.py
    # We need to strip the prefix to get the module path.
    components = Path(f_path).parts
    embed_path = "../../../../../../../" + str(
        Path(*components[1:])
    )  # without `external/` prefix
    module_path = str(Path(*components[2:]))  # without `external/fastapi_src/` prefix

    # Format as a Cap'n Proto module definition.
    if f_path.endswith(".py"):
        modules.append(f'(name = "{module_path}", pythonModule = embed "{embed_path}")')
    elif f_path.endswith(".so"):
        modules.append(f'(name = "{module_path}", data = embed "{embed_path}")')

print(",".join(modules), end="")
