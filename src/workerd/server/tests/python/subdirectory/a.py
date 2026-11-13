import calendar  # noqa: F401  Reads the standard library zip, so that its directory cache exists.
import sys
import zipimport

from js import Response
from subdir.a import x
from workers import WorkerEntrypoint

STDLIB_ZIP = next(p for p in sys.path if p.endswith(".zip"))
# The top level runs before a dedicated snapshot is collected, so this is the zip directory cache
# that the snapshot records.
ZIP_DIRECTORY_CACHE_AT_TOP_LEVEL = zipimport._zip_directory_cache.get(STDLIB_ZIP)


def check_imports(keeps_import_caches):
    try:
        from _cf_internal_snapshot_info import loadedBaselineSnapshot, loadedSnapshot

        restored_dedicated_snapshot = loadedSnapshot and not loadedBaselineSnapshot
    except ImportError:
        # The runtime doesn't provide the module while it collects a snapshot.
        restored_dedicated_snapshot = False

    if restored_dedicated_snapshot:
        # Invalidating the caches drops the cache, and the next import from the zip builds a new
        # one, so the restored cache is still in use exactly when the caches were kept.
        assert ZIP_DIRECTORY_CACHE_AT_TOP_LEVEL is not None
        restored = zipimport._zip_directory_cache.get(STDLIB_ZIP)
        if keeps_import_caches:
            assert restored is ZIP_DIRECTORY_CACHE_AT_TOP_LEVEL
        else:
            assert restored is not ZIP_DIRECTORY_CACHE_AT_TOP_LEVEL

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
        check_imports(getattr(self.env, "KEEPS_IMPORT_CACHES", False))
