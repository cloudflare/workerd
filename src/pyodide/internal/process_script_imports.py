# This script is used to prepare a worker prior to a _package_ memory snapshot being taken.
# All it does is walk through the imports in each of the worker's modules and attempts to import
# them. Local imports are not possible because the worker file path is explicitly removed from the
# module search path.
CF_LOADED_MODULES = []


def _do_it():
    import ast
    from pathlib import Path
    import sys

    def find_imports(source: str) -> list[str]:
        try:
            mod = ast.parse(source)
        except SyntaxError:
            return []
        imports = set()
        for node in ast.walk(mod):
            if isinstance(node, ast.Import):
                for name in node.names:
                    imports.add(name.name)
            elif isinstance(node, ast.ImportFrom):
                module_name = node.module
                if module_name is None:
                    continue
                imports.add(module_name)
        return list(sorted(imports))

    def process_script(script):
        for mod in find_imports(script):
            try:
                __import__(mod)
                CF_LOADED_MODULES.append(mod)
            except ImportError:
                pass

    def process_scripts():
        # Currently this script assumes that it is generating a _package_ snapshot- one that
        # only includes non-vendored packages. Because of this we do not wish to import local
        # modules, the easiest way to ensure they cannot be imported is to remove
        # `/session/metadata` from the sys path.
        worker_files_path = "/session/metadata"
        sys.path.remove(worker_files_path)
        for script in Path(worker_files_path).glob("**/*.py"):
            process_script(script.read_text())
        sys.path.append(worker_files_path)

    process_scripts()


_do_it()
del _do_it
