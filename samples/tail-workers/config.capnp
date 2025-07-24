using Workerd = import "/workerd/workerd.capnp";

const tailWorkerExample :Workerd.Config = (
  services = [
    (name = "my-worker", worker = .myWorker),
    (name = "proxy-worker", worker = .proxyWorker),
    (name = "tail-worker", worker = .tailWorker),
  ],
  sockets = [ ( name = "http", address = "*:8080", http = (), service = "my-worker" ) ],
);

const myWorker :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker.js")
  ],
  bindings = [
    (name = "SERVICE", service = "proxy-worker"),
  ],
  compatibilityDate = "2024-10-14",
  compatibilityFlags = ["experimental"],
  tails = ["proxy-worker"],
);

const proxyWorker :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "proxy-worker.js")
  ],
  bindings = [
    (name = "SERVICE", service = "tail-worker"),
  ],
  compatibilityDate = "2024-10-14",
  compatibilityFlags = ["experimental"],


);

const tailWorker :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "tail.js")
  ],
  compatibilityDate = "2024-10-14",
  compatibilityFlags = ["experimental"],
);
