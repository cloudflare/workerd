using Workerd = import "/workerd/workerd.capnp";

# Two Workers and a Durable Object for `workerd serve --threads --loops`: `front` answers on
# whichever loop accepts the connection, reaches `back` through a service binding, and calls an
# object that runs on the first loop.
const config :Workerd.Config = (
  services = [
    ( name = "front",
      worker = (
        modules = [(name = "worker", esModule = embed "shared-worker.mjs")],
        compatibilityDate = "2025-06-01",
        bindings = [
          (name = "BACK", service = "back"),
          (name = "NS", durableObjectNamespace = "Counter"),
        ],
        durableObjectNamespaces = [
          (className = "Counter", uniqueKey = "shared-counter"),
        ],
        durableObjectStorage = (inMemory = void),
      )
    ),
    ( name = "back",
      worker = (
        modules = [(name = "worker", esModule = embed "shared-worker.mjs")],
        compatibilityDate = "2025-06-01",
      )
    ),
  ],
  sockets = [
    (name = "http", address = "127.0.0.1:0", http = (), service = "front"),
  ],
);
