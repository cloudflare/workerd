using Workerd = import "/workerd/workerd.capnp";

const bundle :Workerd.ExtensionBundle = (
  modules = [
    ( name = "test:builtin-module", esModule = embed "builtin-module.js" ),
    ( name = "test-internal:builtin-internal-module", esModule = embed "builtin-internal-module.js", internal = true ),
  ]
);
