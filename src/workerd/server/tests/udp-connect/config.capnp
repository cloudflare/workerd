# config.capnp
using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    ( name = "main", worker = .worker ),
  ],
  sockets = [
    ( name = "udp", address = "*:0", udp = (idleTimeoutMs = 1000), service = "main" ),
  ]
);

const worker :Workerd.Worker = (
  modules = [
    ( name = "./index.mjs", esModule = embed "index.mjs" )
  ],
  compatibilityDate = "2024-01-01",
  compatibilityFlags = [ "experimental" ],
);
