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
