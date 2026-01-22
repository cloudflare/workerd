load("@aspect_rules_ts//ts:defs.bzl", "ts_config", "ts_project")
load("@rules_shell//shell:sh_test.bzl", "sh_test")

def wd_test(
        src,
        data = [],
        name = None,
        args = [],
        ts_deps = [],
        python_snapshot_test = False,
        generate_default_variant = True,
        generate_all_autogates_variant = True,
        generate_all_compat_flags_variant = True,
        compat_date = "",
        sidecar = None,
        sidecar_port_bindings = [],
        sidecar_randomize_ip = True,
        load_snapshot = None,
        env = {},
        **kwargs):
    """Rule to define tests that run `workerd test` with a particular config.

    Args:
      src: A .capnp config file defining the test. (`name` will be derived from this if not
        specified.) The extension `.wd-test` is also permitted instead of `.capnp`, in order to
        avoid confusing other build systems that may assume a `.capnp` file should be compiled.
      data: Additional files which the .capnp config file may embed. All TypeScript files will be
        compiled, their resulting files will be passed to the test as well. Usually TypeScript or
        Javascript source files.
      args: Additional arguments to pass to `workerd`. Typically used to pass `--experimental`.
      generate_default_variant: If True (default), generate the default variant with oldest compat
        date.
      generate_all_autogates_variant: If True (default), generate @all-autogates variants.
      generate_all_compat_flags_variant: If True (default), generate @all-compat-flags variants.
      compat_date: If specified, use this compat date for the default variant instead of
        2000-01-01. Does not affect the @all-compat-flags variant which always uses 2999-12-31.
      sidecar: If set, an executable that is run in parallel with the test, and provides some
        functionality needed for the test. This is usually a backend server, with workerd serving
        as the client. The sidecar will be killed once the test completes.
      sidecar_port_bindings: A list of binding names which will be filled in with random port
        numbers that the sidecar and test can use for communication. The test will only begin once
        the sidecar is listening to these ports. In the sidecar, access these bindings as
        environment variables. In the wd-test file, add fromEnvironment bindings to expose them to
        the test. Reminder: you'll also need to add a network = ( allow = ["private"] ) service.
      sidecar_randomize_ip: If true (default), a random IP address will be assigned to the sidecar
        process, and provided in the environment variable SIDECAR_HOSTNAME.
      load_snapshot: If specified, a label to a snapshot file to load.
      env: Environment variables to set when running the test.

    The following test variants are generated based on the flags:
      - name@ (if generate_default_variant): oldest compat date (2000-01-01)
      - name@all-compat-flags (if generate_all_compat_flags_variant): newest compat date
        (2999-12-31)
      - name@all-autogates (if generate_all_autogates_variant): all autogates + oldest compat date
    """

    # Sidecar and python_snapshot_test cannot be used together due to complexity.
    # TODO(later): Implement support for combining these two options if we need it.
    if sidecar and python_snapshot_test:
        fail("sidecar and python_snapshot_test cannot be used together")

    # Default name based on src.
    if name == None:
        name = src.removesuffix(".capnp").removesuffix(".wd-test").removesuffix(".ts-wd-test")

    # Compile TypeScript files.
    # Generated declarations are currently not being used, but required based on
    # https://github.com/aspect-build/rules_ts/issues/719
    # TODO(build perf): Consider adopting isolated_typecheck to avoid bottlenecks in TS
    # compilation, see https://github.com/aspect-build/rules_ts/blob/f1b7b83/docs/performance.md#isolated-typecheck.
    # This will require extensive refactoring and we may only want to enable it for some
    # targets, but might be useful if we end up transpiling more code later on.
    ts_srcs = [s for s in data if s.endswith(".ts")]
    if ts_srcs:
        ts_config(
            name = name + "@ts_config",
            src = "tsconfig.json",
            deps = ["@workerd//tools:base-tsconfig"],
        )
        ts_project(
            name = name + "@ts_project",
            srcs = ts_srcs,
            tsconfig = ":" + name + "@ts_config",
            allow_js = True,
            source_map = True,
            declaration = True,
            deps = ["//src/node:node@tsproject"] + ts_deps,
        )
        data = data + [s.removesuffix(".ts") + ".js" for s in ts_srcs]

    # Add workerd binary and source file to data dependencies.
    data = data + [src, "//src/workerd/server:workerd_cross"]

    # Add initial arguments for `workerd test` command.
    base_args = [
        "$(location //src/workerd/server:workerd_cross)",
        "test",
        "$(location {})".format(src),
    ] + args

    # Build environment variables for the test.
    test_env = dict(env)

    # Sidecar configuration.
    # For detailed documentation, see src/workerd/api/node/tests/sidecar-supervisor.mjs
    if sidecar:
        supervisor = "//src/workerd/api/node/tests:sidecar-supervisor"
        data = data + [sidecar, supervisor]
        test_env.update({
            "SIDECAR_COMMAND": "$(location {})".format(sidecar),
            "SIDECAR_SUPERVISOR": "$(location {})".format(supervisor),
            "PORTS_TO_ASSIGN": ",".join(sidecar_port_bindings),
            "RANDOMIZE_IP": "true" if sidecar_randomize_ip else "false",
        })

    # Python snapshot test configuration.
    # We need variants of the test for Python memory snapshot tests. We have to invoke
    # workerd twice, once with --python-save-snapshot to produce the snapshot and once with
    # --python-load-snapshot to use it.
    #
    # We would like to implement this in py_wd_test and not have to complicate wd_test for it, but
    # unfortunately bazel provides no way for a test to create a file that is used by another test.
    # So we cannot do this with two separate `wd_test` rules. We _could_ use a build step to create
    # the snapshot, but then a failure at this stage would be reported as a build failure when
    # really it should count as a test failure. So the only option left is to make this
    # modification to wd_test to invoke workerd twice for snapshot tests.
    if python_snapshot_test:
        test_env["PYTHON_SNAPSHOT_TEST"] = "1"
        test_env["PYTHON_SAVE_SNAPSHOT_ARGS"] = ""

    if load_snapshot:
        data = data + [load_snapshot]
        if python_snapshot_test:
            test_env["PYTHON_SAVE_SNAPSHOT_ARGS"] = "--python-load-snapshot load_snapshot.bin"

    # Define the compat-date args for each variant.
    # Note: dates must be in range [2000-01-01, 2999-12-31] due to parsing constraints.
    default_compat = ["--compat-date={}".format(compat_date or "2000-01-01")]
    newest_compat = ["--compat-date=2999-12-31"]

    # Generate test variants based on the flags.
    variants = []
    if generate_default_variant:
        variants.append((name + "@", default_compat))
    if generate_all_compat_flags_variant:
        variants.append((name + "@all-compat-flags", newest_compat))
    if generate_all_autogates_variant:
        variants.append((name + "@all-autogates", default_compat + ["--all-autogates"]))

    for variant_name, extra_args in variants:
        sh_test(
            name = variant_name,
            srcs = ["//build/fixtures:wd_test.sh"],
            data = data,
            args = base_args + extra_args,
            env = test_env,
            **kwargs
        )
