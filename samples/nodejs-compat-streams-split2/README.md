# Node.js Compat Example

To run the example on http://localhost:8080

```sh
$ bazel-bin/src/workerd/server/workerd serve --experimental $(realpath samples/nodejs-compat-streams-split2/config.capnp)
```

To run using bazel

```sh
$ bazel run //src/workerd/server:workerd -- serve $(realpath samples/nodejs-compat-streams-split2/config.capnp)
```

To create a standalone binary that can be run:

```sh
$ ./workerd compile config.capnp > nodejs-compat

$ ./nodejs-compat
```

To test:

```sh
% curl http://localhost:8080
hello
from
the
wonderful
world
of
node.js
streams!
```
