using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    ( name = "main", worker = .worker ),
  ],
  sockets = [
    ( name = "http", address = "127.0.0.1:0", http = (), service = "main" ),
  ]
);

const worker :Workerd.Worker = (
  compatibilityDate = "2024-01-01",
  compatibilityFlags = ["nodejs_compat", "rtti_api"],
  modules = [
    ( name = "./index.mjs", esModule = embed "../dist/index.mjs" )
  ],
);
