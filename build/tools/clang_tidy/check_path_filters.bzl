"""Per-check path filtering for incremental clang-tidy rollout.

Checks listed here are only enabled for files under specific paths. This allows
incremental rollout of new checks: add the check to .clang-tidy, add an entry
here with an empty list, then add paths as they are cleaned up.

Keys are clang-tidy check names. Values are lists of file path prefixes.
A path prefix matches all files under that directory:
  - "src/workerd/io" matches src/workerd/io/foo.c++ and src/workerd/io/bar/baz.c++

Behavior:
  - Check in .clang-tidy but NOT here: runs everywhere (normal behavior)
  - Check here with empty list: runs nowhere
  - Check here with paths: runs only for files under listed paths

Once a check is fully rolled out, remove its entry to run everywhere.
"""

CHECK_PATH_FILTERS = {
    "workerd-unsafe-continuation-capture": [
        # Add paths here as they are cleaned up:
        # "src/workerd/io",
        # "src/workerd/api",
    ],
}
