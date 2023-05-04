using Workerd = import "/workerd/workerd.capnp";

const helloWorldExample :Workerd.Config = (

  services = [ (name = "main", worker = .helloWorld) ],

  sockets = [ ( name = "http", address = "*:8080", http = (), service = "main" ) ]
);

const helloWorld :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker.js")
  ],
  compatibilityDate = "2022-11-08",
  compatibilityFlags = ["nodejs_compat"]
);
