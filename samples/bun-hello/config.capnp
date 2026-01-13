# Bun Hello World Sample
# Run with: workerd serve config.capnp
# Test with: curl http://localhost:9123

using Workerd = import "/workerd/workerd.capnp";

const bunHelloExample :Workerd.Config = (
  services = [ (name = "main", worker = .bunHello) ],
  sockets = [ ( name = "http", address = "*:9123", http = (), service = "main" ) ]
);

const bunHello :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker.js")
  ],
  compatibilityDate = "2024-01-01",
  compatibilityFlags = ["nodejs_compat"]
);
