# Hello WASM Example

## Build WASM Binary

```sh
$ cargo install worker-build
$ worker-build --release
```

## Run with `workerd`

To run the example on http://localhost:8080

```sh
$ ./workerd serve config.capnp
```

To run using bazel

```sh
$ bazel run //src/workerd/server:workerd -- serve $(pwd)/samples/hello-wasm/config.capnp
```

To create a standalone binary that can be run:

```sh
$ ./workerd compile config.capnp > hellowasm

$ ./hellowasm
```

To test:

```sh
% curl http://localhost:8080
Hello WASM!
```
