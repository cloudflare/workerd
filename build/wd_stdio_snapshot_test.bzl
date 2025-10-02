"""
Macro for stdio snapshot tests that verify stdout/stderr against expected files.
"""

load("@rules_shell//shell:sh_test.bzl", "sh_test")

def wd_stdio_snapshot_test(
        src,
        data = [],
        name = None,
        args = [],
        **kwargs):
    """Rule to define tests that verify workerd output against stdio snapshot files.

    This macro simplifies testing workerd output by automatically inferring
    the expected output file paths from the test name.

    Args:
        src: A .wd-test config file defining the test.
        data: Additional files which the config file may embed (usually .js files).
        name: Test name (derived from src if not specified). Will have "-sh-test" appended.
        args: Additional arguments to pass to workerd (e.g., --experimental).
        **kwargs: Additional arguments to pass to sh_test.
    """

    # Derive base name from src if not provided
    if name == None:
        base_name = src.removesuffix(".wd-test")
        test_name = base_name
    else:
        base_name = name
        test_name = name

    # Construct expected output file names
    expected_stdout = base_name + ".expected_stdout"
    expected_stderr = base_name + ".expected_stderr"

    # Build data dependencies list (automatically include expected output files)
    test_data = [
        expected_stdout,
        src,
        "//src/workerd/server:workerd_cross",
    ] + data

    # Only include stderr file if it exists
    if native.glob([expected_stderr], allow_empty = True):
        test_data.append(expected_stderr)

    # Build args list
    test_args = [
        "$(location //src/workerd/server:workerd_cross)",
        "$(location " + src + ")",
        "$(location " + expected_stdout + ")",
    ]

    # Add stderr file location if it exists, otherwise pass the expected filename (script will check if it exists)
    if native.glob([expected_stderr], allow_empty = True):
        test_args.append("$(location " + expected_stderr + ")")
    else:
        test_args.append(expected_stderr)  # Pass the filename, script will check if file exists

    test_args += args

    sh_test(
        name = test_name,
        srcs = ["//:build/test_stdio_snapshot.sh"],
        args = test_args,
        data = test_data,
        **kwargs
    )
