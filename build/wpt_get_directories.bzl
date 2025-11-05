def wpt_all_dirs():
    always_exclude = [
        ".*/**",  # dotfiles
        "tools/**",  # backend
    ]
    files = native.glob(["**/*"], exclude_directories = 1)
    return native.glob(["**/*"], exclude_directories = 0, exclude = files + always_exclude)
