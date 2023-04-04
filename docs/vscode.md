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

If this command fails, try again after running:

```sh
$ bazel clean --expunge
$ bazel test //...
```

There is an issue between workerd's bazel setup and Hedron's compile_commands.json generator (tracked in
https://github.com/cloudflare/workerd/issues/506).

## VSCode Tasks

There is a `.vscode/tasks.json` file included in the project that is seeded with a few useful
tasks for building and testing.

## Debugging

There is a `.vscode/launch.json` file included in the project that is with a single configuration
for debugging the `workerd` binary on Linux.

The debug from vscode, first ensure that you have saved a vscode workspace for workerd,
`File -> Save Workspace As...`, then `Run -> Start Debugging (F5)`.

Running the project will prompt you for workerd configuration and proceed to serve it from workerd
running under `gdb`. To use the helloworld sample, the configuration argument would be
`serve ${workspaceFolder}/samples/helloworld/config.capnp`.
