# Process 2: target worker. Run with --debug-port=127.0.0.1:9230

using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    ( name = "target", worker = .targetWorker ),
  ],
  sockets = [
    ( name = "http", address = "*:8081", http = (), service = "target" )
  ]
);

const targetWorker :Workerd.Worker = (
  modules = [ ( name = "worker", esModule = embed "target-worker.js" ) ],
  compatibilityDate = "2024-01-01",
  compatibilityFlags = ["experimental"]
);
