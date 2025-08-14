using Workerd = import "/workerd/workerd.capnp";

const helloWorldExample :Workerd.Config = (
  services = [ (name = "main", worker = .helloWorld) ],
  sockets = [ ( name = "http", address = "*:8080", http = (), service = "main" ) ]
);

const helloWorld :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker.ts")
  ],
  compatibilityDate = "2025-08-01",
  # In strip modes only types are removed from the code. Using extra features like enums will cause
  # the worker to fail to load.
  compatibilityFlags = ["typescript_strip_types"]
  # In transpile mode the code is transpiled to JavaScript using fully-feature swc transpiler.
  # compatibilityFlags = ["typescript_transpile"]
);
