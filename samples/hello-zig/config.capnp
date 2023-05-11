using Workerd = import "/workerd/workerd.capnp";

const helloWasmExample :Workerd.Config = (
  services = [ (name = "main", worker = .helloWasm) ],
  sockets = [ ( name = "http", address = "*:8080", http = (), service = "main" ) ]
);

const helloWasm :Workerd.Worker = (
  modules = [
    ( name = "entrypoint", freestandingWasm = embed "./build/index.wasm" )
  ],
  compatibilityDate = "2023-03-14",
);
