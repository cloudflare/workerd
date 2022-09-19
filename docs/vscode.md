# Developing workerd with Visual Studio Code

A few helpful tips for developing workerd with Visual Studio Code.

## clangd

We use [clangd](https://marketplace.visualstudio.com/items?itemName=llvm-vs-code-extensions.vscode-clangd)
for code completion and diagnostics.

For debugging, you will need the [C/C++ extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools).

## Generate a compile_commands.json file

```sh
$ bazel run //:refresh_compile_commands
```

## VSCode Tasks

Create a `.vscode/tasks.json` file with the following contents to create default build
and test tasks:

```json
{
  "version": "2.0.0",
  "tasks": [
    {
      "label": "Bazel Build",
      "type": "shell",
      "command": "bazel",
      "args": [
        "build",
        "//src/workerd/server"
      ],
      "group": {
        "kind": "build",
        "isDefault": true
      },
      "problemMatcher": "$gcc",
      "presentation": {
        "echo": true,
        "reveal": "always",
        "focus": false,
        "panel": "shared",
        "showReuseMessage": false,
        "clear": true
      }
    },
    {
      "label": "Bazel Build (opt)",
      "type": "shell",
      "command": "bazel",
      "args": [
        "build",
        "-c", "opt",
        "//src/workerd/server"
      ],
      "group": {
        "kind": "build",
        "isDefault": false
      },
      "problemMatcher": "$gcc",
      "presentation": {
        "echo": true,
        "reveal": "always",
        "focus": false,
        "panel": "shared",
        "showReuseMessage": false,
        "clear": true
      }
    },
    {
      "label": "Bazel Test",
      "type": "shell",
      "command": "bazel",
      "args": [
        "test",
        "--cache_test_results=no",
        "//..."
      ],
      "group": {
        "kind": "test",
        "isDefault": true
      },
      "problemMatcher": "$gcc",
      "presentation": {
        "echo": true,
        "reveal": "always",
        "focus": false,
        "panel": "shared",
        "showReuseMessage": false,
        "clear": true
      }
    }
  ]
}
```

## Debugging

Create a `.vscode/launch.json` file with the following contents to use vscode's c++ debugging
with workerd:

```json
{
  // Use IntelliSense to learn about possible attributes.
  // Hover to view descriptions of existing attributes.
  // For more information, visit: https://go.microsoft.com/fwlink/?linkid=830387
  "version": "0.2.0",
  "configurations": [
    {
      "name": "workerd debug",
      "preLaunchTask": "Bazel Build",
      "type": "cppdbg",
      "request": "launch",
      "MIMode": "gdb",
      "program": "${workspaceFolder}/bazel-out/k8-fastbuild/bin/src/workerd/server/workerd",
      "args": ["--help"],
      "cwd": "${workspaceFolder}/bazel-out/k8-fastbuild/bin/src/workerd/server/workerd.runfiles/workerd",
      "stopAtEntry": false,
      "externalConsole": false
    }
  ]
}
```

Set the `args` appropriate to debug with specific workerd configurations.
