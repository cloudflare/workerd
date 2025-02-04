using Workerd = import "/workerd/workerd.capnp";

const tailWorkerExample :Workerd.Config = (
  services = [
    (name = "log", worker = .logWorker),
  ],
  sockets = [ ( name = "rpc", address = "*:8081", http = ( capnpConnectHost = "capnp" ), service = "log" ) ],
  autogates = [
    "workerd-autogate-streaming-tail-workers",
  ],
);

const logWorker :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "tail.js")
  ],
  compatibilityDate = "2024-10-14",
);

