import sys
from site import addsitedir

VENDOR_PATH = "/session/metadata/vendor"
PYTHON_MODULES_PATH = "/session/metadata/python_modules"


def setup_python_search_path(*, LEGACY_VENDOR_PATH, PROCESS_PTH_FILES):
    # adjustSysPath adds the session path, but it is immortalised by the memory snapshot. This
    # code runs irrespective of the memory snapshot.
    if VENDOR_PATH in sys.path and LEGACY_VENDOR_PATH:
        sys.path.remove(VENDOR_PATH)

    if PYTHON_MODULES_PATH in sys.path:
        sys.path.remove(PYTHON_MODULES_PATH)

    # Insert vendor path after system paths but before site-packages
    # System paths are typically: ['/session', '/lib/python312.zip', '/lib/python3.12', '/lib/python3.12/lib-dynload']
    # We want to insert before '/lib/python3.12/site-packages' and other site-packages
    #
    # We also need the session path to be before the vendor path, if we don't do so then a local
    # import will pick a module from the vendor path rather than the local path. We've got a test
    # that reproduces this (vendor_dir).
    for i, path in enumerate(sys.path):
        if "site-packages" in path:
            if LEGACY_VENDOR_PATH:
                sys.path.insert(i, VENDOR_PATH)
            sys.path.insert(i, PYTHON_MODULES_PATH)
            break
    else:
        # If no site-packages found, fail
        raise ValueError("No site-packages found in sys.path")

    if PROCESS_PTH_FILES:
        # addsitedir processes any .pth files in PYTHON_MODULES_PATH, allowing
        # packages to extend sys.path declaratively. It also re-adds the
        # directory to sys.path, but it was already inserted above so this is a
        # no-op for that purpose.
        addsitedir(PYTHON_MODULES_PATH)
