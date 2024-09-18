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

## Code Formatting

workerd code is automatically formatted by clang-format. Run `python ./tools/cross/format.py` to reformat the code
or use the appropriate IDE extension.
While workerd generally requires llvm 15, formatting requires clang-format-18.

Code formatting is checked before check-in and during `Linting` CI build.

## Clang-format

Workerd depends on clang-format v18.1.8. In order to install it on Linux, please run the following code:

```bash
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 18 clang-format
```

If you didn't do so, please symlink clang-format-18 to clang.

```bash
sudo ln -s /usr/bin/clang-format-18 /usr/bin/clang-format
```
