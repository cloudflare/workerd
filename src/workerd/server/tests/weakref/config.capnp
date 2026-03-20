# config.capnp
using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  v8Flags = [ "--expose-gc" ],
  services = [
    ( name = "main", worker = .worker ),
  ],
  sockets = [
    ( name = "http", address = "*:0", http = (), service = "main" ),
  ]
);

const worker :Workerd.Worker = (
  modules = [
    ( name = "./index.mjs", esModule = embed "index.mjs" )
  ],
  compatibilityFlags = ["enable_weak_ref"],
);

