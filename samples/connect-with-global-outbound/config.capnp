using Workerd = import "/workerd/workerd.capnp";

const helloWorldExample :Workerd.Config = (
  services = [ (name = "main", worker = .helloWorld), (name = "outbound", worker = .outbound) ],
  sockets = [ ( name = "http", address = "*:8080", http = (), service = "main" ) ]
);

const helloWorld :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker.js")
  ],
  compatibilityDate = "2023-02-28",
  globalOutbound = "outbound"
);

const outbound :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "outbound.js")
  ],
  compatibilityDate = "2023-02-28",
);
