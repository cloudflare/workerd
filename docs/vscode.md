# Developing workerd with Visual Studio Code

A few helpful tips for developing workerd with Visual Studio Code.

## clangd

We use [clangd](https://marketplace.visualstudio.com/items?itemName=llvm-vs-code-extensions.vscode-clangd)
for code completion and diagnostics.

For debugging, you will need the [Microsoft C/C++ extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools) installed.

The Microsoft C/C++ extension has IntelliSense support that is not compatible with the clangd extension. We recommend disabling the
Microsoft IntelliSense Engine for this project (`Settings -> C_Cpp.intelliSenseEngine -> disabled`).

For clangd to work well, you need to generate a `compile_commands.json` file. This can be done from within VSCode using
`Run Task -> Generate compile_commands.json` or at the command-line:

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

The [.vscode/tasks.json](../.vscode/tasks.json) file provides a few useful tasks for use within VSCode:

* Bazel build workerd (dbg)
* Bazel build workerd (fastbuild)
* Bazel build workerd (opt)
* Bazel build all (dbg)
* Bazel run all tests
* Generate compile_commands.json
* Generate rust-project.json

## Debugging

There are workerd debugging targets within Visual Studio Code which are supported on Linux (x64), OS X (arm64), and Windows (x64).

The [.vscode/launch.json](../.vscode/launch.json) file has launch targets to that can be debugged within VSCode.

Before you start debugging, ensure that you have saved a vscode workspace for workerd,
`File -> Save Workspace As...`, then `Run -> Start Debugging (F5)`. For more information about workspaces, see https://code.visualstudio.com/docs/editor/workspaces.

The main targets of interest are:

* `workerd debug`
* `workerd debug with inspector enabled`
* `workerd test case`

Launching either `workerd debug` or `workerd debug with inspector enabled` will prompt for a workerd configuration for
workerd to serve, the default is `${workspaceFolder}/samples/helloworld/config.capnp`.

Launching `workerd test case` will prompt for a test to debug, the default is `bazel-bin/src/workerd/jsg/jsg-test`.

