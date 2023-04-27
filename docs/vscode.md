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
# Build workerd to ensure all generated sources are present.
$ bazel build //...
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

The [.vscode/tasks.json](../.vscode/tasks.json) file included provides a few useful tasks for use within VSCode:

* Bazel build workerd (dbg)
* Bazel build workerd (fastbuild)
* Bazel build workerd (opt)
* Bazel build all (dbg)
* Bazel run all tests

## Debugging

There are workerd debugging targets within Visual Studio Code. These
are supported on Linux (x64) and OS X (arm64).

The file [.vscode/launch.json](../.vscode/launch.json) has launch targets to that can be debugged within VSCode.

Before you start debugging, ensure that you have saved a vscode workspace for workerd,
`File -> Save Workspace As...`, then `Run -> Start Debugging (F5)`.

The main targets of interest are:

* `workerd debug`
* `workerd debug with inspector enabled`
* `workerd test case`

Launching either `workerd debug` or `workerd debug with inspector enabled` will prompt for a workerd configuration to launch
workerd with, the default is `${workspaceFolder}/samples/helloworld/config.capnp`.

Launching `workerd test case` will prompt for a test to debug, the default is `bazel-bin/src/workerd/jsg/jsg-test`.

