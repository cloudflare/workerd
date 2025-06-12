using Workerd = import "/workerd/workerd.capnp";

const tailWorkerExample :Workerd.Config = (
  services = [
    (name = "main", worker = .mainWorker),
    (name = "proxy", worker = .proxyWorker),
    (name = "tail", worker = .tailWorker),
  ],
  sockets = [ ( name = "http", address = "*:8080", http = (), service = "main" ) ],
);

const mainWorker :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker.js")
  ],
  compatibilityDate = "2025-05-01",
  tails = ["proxy"],
);

const proxyWorker :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "proxy.js")
  ],
  compatibilityDate = "2025-05-01",
  bindings = [
    (name = "TAIL_WORKER", service = "tail"),
  ],
);

const tailWorker :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "tail.js")
  ],
  compatibilityDate = "2025-05-01",
);
