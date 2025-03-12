using Workerd = import "/workerd/workerd.capnp";

const tailWorkerExample :Workerd.Config = (
  services = [
    (name = "worker-a", worker = .workerA),
    (name = "proxy-worker", worker = .proxyWorker),
    (name = "user-worker", worker = .userWorker),
  ],
  sockets = [ ( name = "http", address = "*:8080", http = (), service = "worker-a" ) ],
);

const workerA :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker-a.js")
  ],
  compatibilityDate = "2025-01-01",
  bindings = [
    (name = "PROXY", service = ( name = "proxy-worker")),
    (name = "USER", service = ( name = "user-worker"))

  ]
);

const proxyWorker :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "proxy-worker.js")
  ],
  compatibilityDate = "2025-01-01",
  bindings = [
    (name = "USER", service = ( name = "user-worker"))
  ]
);

const userWorker :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "user-worker.js")
  ],
  compatibilityDate = "2025-01-01",
);
