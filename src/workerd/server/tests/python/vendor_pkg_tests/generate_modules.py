# This script reads the list of files from a zip file and outputs a list of Cap'n Proto module
# definitions to be used in a .wd-test for Python tests.
import sys
from pathlib import Path

# Handle response file format (starting with @) to avoid Windows command line length limits
file_paths = []
for arg in sys.argv[1:]:
    if arg.startswith("@"):
        # Read file paths from response file (space-separated)
        response_file = arg[1:]
        with Path.open(response_file) as f:
            content = f.read().strip()
            if content:
                file_paths.extend(content.split())
    else:
        file_paths.append(arg)

modules = []
for f_path in file_paths:
    # The path from bazel is relative to the exec root, e.g.:
    # external/fastapi_src/fastapi/__init__.py
    # We need to strip the prefix to get the module path.
    #
    # On Windows, we replace windows-style path separators with standard posix path separators.
    components = Path(f_path).parts
    # without `external/` prefix
    embed_path = ("../" * 7) + str(Path(*components[1:])).replace("\\", "\\\\")
    # without `external/fastapi_src/` prefix
    module_path = str(Path("python_modules", *components[2:])).replace("\\", "/")

    # Format as a Cap'n Proto module definition.
    if f_path.endswith(".py"):
        modules.append(f'(name = "{module_path}", pythonModule = embed "{embed_path}")')
    elif f_path.endswith(".so"):
        modules.append(f'(name = "{module_path}", data = embed "{embed_path}")')

print(",".join(modules), end="")
