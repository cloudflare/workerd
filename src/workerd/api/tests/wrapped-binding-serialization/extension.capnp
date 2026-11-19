using Workerd = import "/workerd/workerd.capnp";

# Provides an internal wrapper module used to build a "wrapped binding" in the test config. Wrapped
# bindings can only be produced by internal modules, so we register one via an extension.
const extension :Workerd.Extension = (
  modules = [
    ( name = "test:wrapper", esModule = embed "wrapper.js", internal = true ),
  ]
);
