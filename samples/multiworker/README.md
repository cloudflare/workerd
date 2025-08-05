# Node.js Compat Example

> Make sure you build `rpc.js` before running this sample

To run using bazel

```sh
$ bazel run //src/workerd/server:workerd -- serve ~/cloudflare/workerd/samples/nodejs-compat/config.capnp
```

and in another terminal

```sh
$ bazel run //src/workerd/server:workerd -- serve ~/cloudflare/workerd/samples/nodejs-compat/config-remote.capnp
```

To test:

```sh
% curl http://localhost:8080
{"pipelining":"qux","function":1,"functionArg":5,"rpcTarget":-4,"rpcTargetPipeline":3}
```
