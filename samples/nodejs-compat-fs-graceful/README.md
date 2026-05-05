# Node.js 'node:fs' + `graceful-fs` Compat Example

Demonstrates the use of the `node:fs` built-in and the use of the `graceful-fs`
module from npmjs being used in a worker without the need for any polyfills or
modifications to `graceful-fs`.

To run the example on http://localhost:8080

```sh
$ ./workerd serve config.capnp
```

To run using bazel

```sh
$ bazel run //src/workerd/server:workerd -- serve ~/cloudflare/workerd/samples/nodejs-compat/config.capnp
```

To create a standalone binary that can be run:

```sh
$ ./workerd compile config.capnp > nodejs-compat

$ ./nodejs-compat
```

To test:

```sh
% curl http://localhost:8080
Hello World
```
