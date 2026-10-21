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
