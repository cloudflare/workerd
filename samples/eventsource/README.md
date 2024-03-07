# EventSource Example

To run the example on http://localhost:8080

```sh
$ ./workerd serve config.capnp
```

To run using bazel

```sh
$ bazel run //src/workerd/server:workerd -- serve ~/cloudflare/workerd/samples/eventsource/config.capnp
```
