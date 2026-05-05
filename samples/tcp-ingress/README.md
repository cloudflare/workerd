# REPL Server

This sample contains a simple TCP server based on the connect() handler. When a connection gets
established on port 8081, it simply pipes the input stream to the output.

## How to use
```
./bazel-bin/src/workerd/server/workerd serve samples/tcp-ingress/config.capnp --experimental
echo "Hello World!" | nc localhost 8081
```
