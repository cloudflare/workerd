# Development

## Clangd

To get language support in VSCode and other IDEs we recommend using `clangd`-based language server.
The server reads supplied `compile_flags.txt` file for correct options and include paths to resolve symbols
in opened files.

To support `clangd` project-level operations like `Find References` `compile_commands.json` file needs to
be generated that will list all files in the project. If you do this, then it needs to be re-generated periodically
when new files appear.

1.  Install `gen-compile-commands`

```sh
just prepare
```

2.  Generate `compile_commands.json`

```sh
just compile-commands
```

## Dependencies

In order to install dependencies, please run the following command. Also refer to Building workerd in README.md for the list of required dependencies by platform.

- For Ubuntu:

```sh
just prepare-ubuntu
```

## Remote execution

Cloudflare employees can build and test on bazelflare, which runs every action on Linux x86_64
executors and shares its cache with CI. Log in once with
`cloudflared access login https://bazelflare-workerd.runtime-playground.workers.dev`, then add
`--config=bazelflare` (or `bazelflare-asan`, `bazelflare-tsan`) to a `bazel build` or `bazel test`
command, from macOS or Linux. Tests that need the network or a container engine run on executors
that have them; the container tests are `enormous`, which Bazel skips without `--test_size_filters=`.

A bazelflare build downloads only the outputs it needs locally, so the generated headers and Rust
sources that clangd and rust-analyzer read from `bazel-bin` (Cap'n Proto and cxx bridge code, the
`_virtual_includes` trees) stay remote. `just fetch-generated [targets]` downloads them, by default
for `//src/...`, without compiling or downloading anything else. A later build that regenerates one
of them removes the local copy; rerun the command to download it again.

To download them on every build instead, add `--config=bazelflare-generated` to a bazelflare build,
or enable it for all of them by adding this line to `.local/bazelrc`, which `.bazelrc` imports and
git ignores:

```
common:bazelflare --config=bazelflare-generated
```

The config adds the output group of `just fetch-generated` to the build and downloads each generated
header and Rust source that it produces or finds missing locally, so the files of the targets you
build stay up to date. It doesn't change action keys, so builds still share the cache with CI. With
the line above, `just fetch-generated` isn't needed, and it and `--config=bazelflare-generated` fail:
Bazel rejects the config's aspect when it is added twice.

## Formatting and Linting

`just format` formats the repository using clang-format, prettier, ruff, buildifier, and rustfmt.
`tools/cross/format.json` defines which formatter applies to which files.

```sh
just format                    # everything
just format path/to/file ...   # specific files
just format-head               # files changed since HEAD~
```

The formatters are built by Bazel at pinned versions, so nothing needs to be installed beyond what
`just prepare` sets up. To check without modifying files, run `python3 tools/cross/format.py --check`.

### Pre-commit hook

To enable the pre-commit hook, run:

```sh
git config core.hooksPath githooks
```

The hook rejects commits that add `KJ_DBG` and checks formatting of staged files. Skip it with
`git commit --no-verify`.

### CI

CI checks formatting with `format.py --check`, runs clippy and rustfmt via `--config=lint`, and runs
ESLint as `<target>@eslint` test targets.

### Other linters

- `just lint`: ESLint
- `just clippy [package]`: Rust clippy; see [src/rust/AGENTS.md](../src/rust/AGENTS.md)
- `just clang-tidy [target]`: clang-tidy with workerd's custom checks; see
  [build/AGENTS.md](../build/AGENTS.md)
