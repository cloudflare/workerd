# Developing workerd with Visual Studio Code

A few helpful tips for developing workerd with Visual Studio Code.

## clangd

We use [clangd](https://marketplace.visualstudio.com/items?itemName=llvm-vs-code-extensions.vscode-clangd)
for code completion and diagnostics.

For debugging, you will need the [C/C++ extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools).

The Microsoft C/C++ Intellisense plugin is not compatible with the clangd extension. We recommend disabling the
Microsoft Intellisense extension for this project.

## Generate a compile_commands.json file

```sh
$ bazel run //:refresh_compile_commands
```

## VSCode Tasks

There is a `.vscode/tasks.json` file included in the project that is seeded with a few useful
tasks for building and testing.

## Debugging

There is a `.vscode/launch.json` file included in the project that is with a single configuration
for debugging the `workerd` binary.
