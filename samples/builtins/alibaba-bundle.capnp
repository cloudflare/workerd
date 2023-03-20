using Workerd = import "/workerd/workerd.capnp";

const bundle :Workerd.BuiltinsBundle = (
  modules = [
    ( name = "alibaba:cave", esModule = embed "cave.js" ),
    ( name = "alibaba-internal:secrets", esModule = embed "secrets.js", internal = true ),
  ]
);
