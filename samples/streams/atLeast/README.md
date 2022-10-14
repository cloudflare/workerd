# Hello World Example

To run the example on http://localhost:8080

```sh
$ ./workerd serve config.capnp
```

To run using bazel

```sh
$ bazel run //src/workerd/server:workerd -- serve ~/cloudflare/workerd/samples/streams/atLeast/config.capnp
```

To create a standalone binary that can be run:

```sh
$ ./workerd compile config.capnp > streams

$ ./streams
```

To test:

```sh
% curl http://localhost:8080
OK
```
