# Extensions

This directory contains comprehensive samples of using workerd extensions.

This example defines a fictional burrito shop extension in
[burrito-shop.capnp](burrito-shop.capnp)
and demonstrates following features:

- using modules to provide new user-level api: [burrito-shop.js](burrito-shop.js) and
  [worker.js](worker.js)
- using internal modules to hide implementation details from the user: [kitchen.js](kitchen.js)

The sample will be extended as more functionality is implemented.


## Running

```
$ bazel run //src/workerd/server:workerd -- serve $(pwd)/samples/extensions/config.capnp
$ curl localhost:8080 -X POST -d 'veggie'
9
```
