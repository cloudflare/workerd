# Web Streams Example

Demonstrates Web Streams API features including custom ReadableStream, TransformStream,
and byte streams.

## Endpoints

- `/sync` - Default stream with uppercase transform (synchronous)
- `/async` - Default stream with uppercase transform (with delays)
- `/bytes/sync` - Byte stream, no transform (synchronous)
- `/bytes/async` - Byte stream, no transform (with delays)

## Running

```sh
$ bazel run //src/workerd/server:workerd -- serve $(pwd)/samples/web-streams/config.capnp
```

Or with a local workerd binary:

```sh
$ ./workerd serve config.capnp
```

## Testing

```sh
$ curl http://localhost:8080/sync
$ curl http://localhost:8080/async
$ curl http://localhost:8080/bytes/sync
$ curl http://localhost:8080/bytes/async
```
