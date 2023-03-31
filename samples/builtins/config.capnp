using Workerd = import "/workerd/workerd.capnp";
using AlibabaBundle = import "alibaba-bundle.capnp";

const helloWorldExample :Workerd.Config = (
  services = [ (name = "main", worker = .helloWorld) ],
  sockets = [ ( name = "http", address = "*:8080", http = (), service = "main" ) ],
  extensions = [ AlibabaBundle.bundle ],
);

const helloWorld :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker.js")
  ],
  compatibilityDate = "2022-09-16",
);
