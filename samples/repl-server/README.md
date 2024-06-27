# REPL Server

This sample creates a simple REPL server which communicates with a Node.js
client over HTTP. This is very experimental and is only included for testing
purposes for now.

## How to use
```
./bazel-bin/src/workerd/server/workerd serve samples/repl-server/config.capnp --experimental
node samples/repl-server/client.js
```
