using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    (name = "main", worker = .main),
    (name = "broken", worker = .broken),
  ],
  sockets = [
    (name = "http", address = "localhost:1", http = (), service = "mainn"),
  ],
);

const main :Workerd.Worker = (
  compatibilityDate = "2025-01-01",
  modules = [(name = "main.js", esModule = embed "ok.js")],
  bindings = [(name = "OTHER", service = "mian")],
);

const broken :Workerd.Worker = (
  compatibilityDate = "2025-01-01",
  modules = [(name = "main.js", esModule = embed "throws.js")],
);
