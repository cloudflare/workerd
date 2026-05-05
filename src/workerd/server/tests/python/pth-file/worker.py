import sys

# The .pth file added a relative path "extra_dir" which should resolve to
# /session/metadata/python_modules/extra_dir and be on sys.path.
expected_extra_dir = "/session/metadata/python_modules/extra_dir"
assert expected_extra_dir in sys.path

# And the module living in that directory must be importable.
import extra_module

assert extra_module.VALUE == 42


def test():
    pass
