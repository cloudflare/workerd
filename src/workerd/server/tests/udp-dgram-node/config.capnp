# config.capnp
using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    ( name = "main", worker = .worker ),
  ],
  sockets = [
    ( name = "udp", address = "127.0.0.1:0", udp = (idleTimeoutMs = 200), service = "main" ),
  ]
);

const worker :Workerd.Worker = (
  modules = [
    ( name = "./index.mjs", esModule = embed "index.mjs" )
  ],
  compatibilityDate = "2026-03-01",
  compatibilityFlags = [ "nodejs_compat", "experimental" ],
);
