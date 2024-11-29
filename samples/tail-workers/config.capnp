using Workerd = import "/workerd/workerd.capnp";

const tailWorkerExample :Workerd.Config = (
  services = [
    (name = "main", worker = .helloWorld),
    (name = "log", worker = .logWorker),
  ],
  sockets = [ ( name = "http", address = "*:8080", http = (), service = "main" ) ]
);

const helloWorld :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker.js")
  ],
  compatibilityDate = "2024-10-14",
  tails = ["log"],
);

const logWorker :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "tail.js")
  ],
  compatibilityDate = "2024-10-14",
);
