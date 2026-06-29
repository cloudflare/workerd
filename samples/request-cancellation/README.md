# Request Cancellation Example

To run the example on http://localhost:8080

```sh
$ ./workerd serve config.capnp
```

To run using bazel

```sh
$ bazel run //src/workerd/server:workerd -- serve ~/cloudflare/workerd/samples/request-cancellation/config.capnp
```

To create a standalone binary that can be run:

```sh
$ ./workerd compile config.capnp > request-cancellation

$ ./request-cancellation
```

To test:

```sh
% curl --max-time 1 http://localhost:8080
Request was aborted
```
