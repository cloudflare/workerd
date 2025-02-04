using Workerd = import "/workerd/workerd.capnp";

const tailWorkerExample :Workerd.Config = (
  services = [
    (name = "main", worker = .helloWorld),
    (name = "log", external = ( address = "127.0.0.1:8081", http = ( capnpConnectHost = "capnp" ))),
  ],
  sockets = [ ( name = "http", address = "*:8080", http = (), service = "main" ) ],
  autogates = [
    "workerd-autogate-streaming-tail-workers",
  ],
);

const helloWorld :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker.js")
  ],
  compatibilityDate = "2024-10-14",
  tails = ["log"],
  bindings = [(name = "log", service = "log")]
);
