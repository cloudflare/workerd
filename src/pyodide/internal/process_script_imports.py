def _do_it():
    import ast
    from pathlib import Path

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
            except ImportError:
                pass

    def process_scripts():
        for script in Path("/session/metadata").glob("**/*.py"):
            process_script(script.read_text())

    process_scripts()


_do_it()
del _do_it
