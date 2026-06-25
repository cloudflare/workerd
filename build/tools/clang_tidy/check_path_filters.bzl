"""Per-check path filtering for incremental clang-tidy rollout.

Checks listed here are only enabled in specific Bazel packages. This allows
incremental rollout of new checks: add the check to .clang-tidy, add an entry
here with an empty list, then add packages as they are cleaned up.

Keys are clang-tidy check names. Values are lists of Bazel package prefixes.
A package prefix matches itself and all subpackages:
  - "//src/workerd/io" matches //src/workerd/io:* and //src/workerd/io/foo:*

Behavior:
  - Check in .clang-tidy but NOT here: runs everywhere (normal behavior)
  - Check here with empty list: runs nowhere
  - Check here with packages: runs only in listed packages

Once a check is fully rolled out, remove its entry to run everywhere.
"""

CHECK_PATH_FILTERS = {
    "workerd-unsafe-continuation-capture": [
        # Add packages here as they are cleaned up:
        # "//src/workerd/io",
        # "//src/workerd/api",
    ],
}
