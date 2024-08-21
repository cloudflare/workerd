# config.capnp
using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
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
  compatibilityDate = "2024-01-01",
  compatibilityFlags = ["nodejs_compat"],
);
