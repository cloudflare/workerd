import sys
import zipimport

from js import Response
from subdir.a import x
from workers import WorkerEntrypoint


def check_import_caches():
    # Check `_zip_directory_cache` is still in the restored state after loading baseline/dedicated snapshot
    # invalidate_caches() clears it, and re-generating it is expensive,
    # so we check it's still there after restore
    stdlib_zip = next(p for p in sys.path if p.endswith(".zip"))
    assert stdlib_zip in zipimport._zip_directory_cache, sorted(
        zipimport._zip_directory_cache
    )

    # A worker file the top level didn't import.
    from subdir.b import y

    assert y == 8

    # A standard library module that nothing imports earlier.
    import colorsys

    assert colorsys.rgb_to_hsv(1, 0, 0) == (0, 1, 1)


class Default(WorkerEntrypoint):
    def fetch(self, request):
        return Response.new("hello world")

    def test(self):
        print("Hi there, this is a test", x)
        check_import_caches()
