using Workerd = import "/workerd/workerd.capnp";

const helloWorldExample :Workerd.Config = (
  services = [
    ( name = "main", worker = .helloWorld ),
    ( name = "server", worker = .server ),
  ],
  sockets = [ ( name = "http", address = "*:8080", http = (), service = "main" ) ],
);

const helloWorld :Workerd.Worker = (
  modules = [
    ( name = "worker", esModule = embed "worker.js" ),
    ( name = "jsonRpc", esModule = embed "jsonRpc.js" )
  ],
  compatibilityDate = "2022-09-16",
  bindings = [
    ( name = "server", service = "server", wrapWith = "jsonRpc" )
  ],
);

const server :Workerd.Worker = (
  modules = [
    ( name = "server", esModule = embed "server.js" ),
    ( name = "jsonRpc", esModule = embed "jsonRpc.js" )
  ],
  compatibilityDate = "2022-09-16",
  unwrapWith = "jsonRpc",
);
