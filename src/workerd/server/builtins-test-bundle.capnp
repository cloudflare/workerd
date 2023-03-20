using Workerd = import "/workerd/workerd.capnp";

const bundle :Workerd.BuiltinsBundle = (
  modules = [
    ( name = "test:builtin-module", esModule = embed "builtin-module.js" ),
    ( name = "test-internal:builtin-internal-module", esModule = embed "builtin-internal-module.js", internal = true ),
  ]
);
