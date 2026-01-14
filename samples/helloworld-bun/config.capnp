# Bun Hello World
# Run: workerd serve config.capnp
# Test: curl http://localhost:9124/

using Workerd = import "/workerd/workerd.capnp";

const helloworldBunExample :Workerd.Config = (
  services = [ (name = "main", worker = .bunWorker) ],
  sockets = [ ( name = "http", address = "*:9124", http = (), service = "main" ) ]
);

const bunWorker :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker.ts"),
    (name = "./bun-bundle.js", esModule = embed "../../dist/bun/bun-bundle.js")
  ],
  compatibilityDate = "2024-09-02",
  compatibilityFlags = ["nodejs_compat_v2"]
);
