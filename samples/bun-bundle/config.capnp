# Bun Bundle Sample
# Demonstrates Bun APIs using the bundled compatibility layer
# Works with stock workerd (no native bun:* support needed)
#
# Run with: workerd serve config.capnp
# Test with: curl http://localhost:9124

using Workerd = import "/workerd/workerd.capnp";

const bunBundleExample :Workerd.Config = (
  services = [ (name = "main", worker = .bunWorker) ],
  sockets = [ ( name = "http", address = "*:9124", http = (), service = "main" ) ]
);

const bunWorker :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker.js"),
    (name = "./bun-bundle.js", esModule = embed "bun-bundle.js")
  ],
  compatibilityDate = "2024-01-01",
  compatibilityFlags = ["nodejs_compat"]
);
