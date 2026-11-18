using Workerd = import "/workerd/workerd.capnp";

# Two Workers for `workerd serve --threads`: `front` answers with the identity of its isolate
# and, through a service binding, that of the `back` it reaches.
const config :Workerd.Config = (
  services = [
    ( name = "front",
      worker = (
        modules = [(name = "worker", esModule = embed "worker.mjs")],
        compatibilityDate = "2025-06-01",
        bindings = [(name = "BACK", service = "back")],
      )
    ),
    ( name = "back",
      worker = (
        modules = [(name = "worker", esModule = embed "worker.mjs")],
        compatibilityDate = "2025-06-01",
      )
    ),
  ],
  sockets = [
    (name = "http", address = "127.0.0.1:0", http = (), service = "front"),
  ],
);
