from workers import WorkerEntrypoint

# Without the fix, this failed with:
#
# service default-class-with-legacy-global-handlers: Uncaught TypeError: Cannot define multiple default entrypoints
#   at pyodide:python-entrypoint-helper:286:15 in handleDefaultClass
#   at pyodide:python-entrypoint-helper:321:5 in initPython


class Default(WorkerEntrypoint):
    def test(*args):
        pass
