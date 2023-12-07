using Workerd = import "/workerd/workerd.capnp";

const extension :Workerd.Extension = (
  modules = [
    ( name = "test:module", esModule = embed "module.js" ),
  ]
);
