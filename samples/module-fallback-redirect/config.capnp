using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    (name = "main", worker = .worker),
  ],
  sockets = [
    (name = "http", address = "127.0.0.1:8080", http = (), service = "main"),
  ],
);

const worker :Workerd.Worker = (
  modules = [
    (name = "worker.js", esModule = embed "worker.js"),
  ],
  compatibilityDate = "2026-08-03",
  compatibilityFlags = ["new_module_registry"],
  moduleFallback = "127.0.0.1:8888",
);
