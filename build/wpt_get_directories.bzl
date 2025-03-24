def wpt_get_directories(root, excludes = []):
    """
    Globs for files within a WPT directory structure, starting from root.
    In addition to an explicitly provided excludes argument, hidden directories
    and top-level files are also excluded as they don't contain test content.
    """

    root_pattern = "{}/*".format(root) if root else "*"
    return native.glob(
        [root_pattern],
        exclude = native.glob(
            [root_pattern],
            exclude_directories = 1,
        ) + [".*"] + excludes,
        exclude_directories = 0,
    )
