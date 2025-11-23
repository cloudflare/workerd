To run:

```bash
$ bazel run --config=release @workerd//src/workerd/server:workerd -- serve $PWD/deps/workerd/samples/python-benchmark/config.capnp

$ wrk -t4 -c64 -d30s --latency -s bench.lua http://127.0.0.1:8080

$ curl -v -X POST http://127.0.0.1:8080
```
