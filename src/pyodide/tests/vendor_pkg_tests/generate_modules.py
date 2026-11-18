# This script reads the list of files from a zip file and outputs a list of Cap'n Proto module
# definitions to be used in a .wd-test for Python tests.
import argparse
from pathlib import Path


class MyArgumentParser(argparse.ArgumentParser):
    def convert_arg_line_to_args(self, arg_line):
        return arg_line.split()


def get_parser():
    parser = MyArgumentParser(
        description="Generate Cap'n Proto module definitions for Python tests",
        fromfile_prefix_chars="@",
    )
    parser.add_argument(
        "--package-depth",
        type=int,
        required=True,
        help="Number of path components in the Bazel package of the generated config",
    )
    parser.add_argument(
        "--template",
        type=str,
        help="Template file",
    )
    parser.add_argument(
        "--out",
        type=str,
        help="Output file",
    )
    parser.add_argument(
        "file_paths",
        nargs="*",
        help="List of file paths or @response_file containing space-separated paths",
    )
    return parser


def make_module_list(file_paths, package_depth):
    modules = []
    for f_path in file_paths:
        if Path(f_path).is_dir():
            continue
        # The path from bazel is relative to the exec root, e.g.:
        # external/fastapi_src/fastapi/__init__.py
        # We need to strip the prefix to get the module path.
        #
        # On Windows, we replace windows-style path separators with standard posix path separators.
        components = Path(f_path).parts
        # without `external/` prefix. The embed is relative to the generated config, so climb out
        # of its package to the main repository, then once more to the external repositories
        # beside it.
        parents = package_depth + 1
        embed_path = ("../" * parents) + str(Path(*components[1:])).replace(
            "\\", "\\\\"
        )
        # without `external/fastapi_src/` prefix
        module_path = str(Path("python_modules", *components[2:])).replace("\\", "/")

        # Format as a Cap'n Proto module definition.
        if f_path.endswith(".py"):
            modules.append(
                f'(name = "{module_path}", pythonModule = embed "{embed_path}")'
            )
        else:
            modules.append(f'(name = "{module_path}", data = embed "{embed_path}")')
    return ",\n".join(modules) + ",\n"


def write_output(modules, template_path, outfile):
    template = Path(template_path).read_text()
    result = template.replace("%PYTHON_VENDORED_MODULES%", modules)
    Path(outfile).write_text(result)


def main():
    parser = get_parser()
    args = parser.parse_args()
    file_paths = args.file_paths
    outfile = args.out
    template = args.template
    modules = make_module_list(file_paths, args.package_depth)
    write_output(modules, template, outfile)


if __name__ == "__main__":
    main()
