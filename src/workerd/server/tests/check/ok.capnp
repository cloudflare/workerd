using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    (name = "main", worker = .main),
    (name = "other", worker = .other),
  ],
  sockets = [
    # Port 1 is privileged: `check` binds no socket, so it never tries.
    (name = "http", address = "localhost:1", http = (), service = "main"),
    # No address: `serve` needs --socket-addr, which `check` warns about.
    (name = "admin", http = (), service = "other"),
  ],
);

const main :Workerd.Worker = (
  compatibilityDate = "2025-01-01",
  modules = [(name = "main.js", esModule = embed "ok.js")],
  bindings = [(name = "OTHER", service = "other")],
);

const other :Workerd.Worker = (
  compatibilityDate = "2025-01-01",
  modules = [(name = "main.js", esModule = embed "ok.js")],
);
