import sys
import unittest
from contextlib import ExitStack
from pathlib import Path
from unittest.mock import MagicMock, patch

DIR = Path(__file__).parent

sys.path.append(str(DIR))
sys.path.append(str(DIR / "workers-api/src"))


class TestIntrospection(unittest.TestCase):
    @classmethod
    def setup_gen(cls):
        with ExitStack() as stack:
            stack.enter_context(patch.dict("sys.modules", js=MagicMock()))
            stack.enter_context(
                patch.dict("sys.modules", _pyodide_entrypoint_helper=MagicMock())
            )
            stack.enter_context(
                patch.dict("sys.modules", {"pyodide": MagicMock(__version__=2)})
            )
            stack.enter_context(
                patch.dict("sys.modules", {"pyodide.code": MagicMock()})
            )
            stack.enter_context(patch.dict("sys.modules", {"pyodide.ffi": MagicMock()}))
            stack.enter_context(
                patch.dict("sys.modules", {"pyodide.http": MagicMock()})
            )
            yield

    @classmethod
    def setUpClass(cls):
        cls.gen = cls.setup_gen()
        next(cls.gen)

    @classmethod
    def tearDownClass(cls):
        next(cls.gen, None)

    def test_collect_methods(self):
        from introspection import collect_methods

        class A:
            x = 2

            @staticmethod
            def f():
                pass

            @classmethod
            def g(cls):
                pass

            @property
            def y(self):
                return 7

            async def fetch(request):
                pass

        self.assertEqual(collect_methods(A), ["fetch"])

        class B(A):
            def some_method(self):
                pass

        self.assertEqual(collect_methods(B), ["fetch", "some_method"])

        from workers import WorkerEntrypoint

        class C(WorkerEntrypoint, B):
            def third_method(self):
                pass

        self.assertEqual(collect_methods(C), ["fetch", "some_method", "third_method"])

        class D(B, WorkerEntrypoint):
            def third_method(self):
                pass

        self.assertEqual(collect_methods(C), ["fetch", "some_method", "third_method"])


if __name__ == "__main__":
    unittest.main()
