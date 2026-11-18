using Workerd = import "/workerd/workerd.capnp";

# A Worker with Durable Objects for `workerd serve --threads`: `front` answers on whichever
# thread accepts the connection and calls its objects, which run on the server's first thread.
const config :Workerd.Config = (
  services = [
    ( name = "front",
      worker = (
        modules = [(name = "worker", esModule = embed "durable-worker.mjs")],
        compatibilityDate = "2025-06-01",
        compatibilityFlags = ["experimental", "enable_ctx_exports", "allow_irrevocable_stub_storage", "unsafe_module"],
        bindings = [
          (name = "COUNTER", durableObjectNamespace = "Counter"),
          (name = "EPHEMERAL", durableObjectNamespace = "Ephemeral"),
          (name = "BACK", service = "back"),
        ],
        durableObjectNamespaces = [
          (className = "Counter", uniqueKey = "counter", enableSql = true),
          (className = "Ephemeral", ephemeralLocal = void),
        ],
        durableObjectStorage = (localDisk = "objects"),
      )
    ),
    ( name = "back",
      worker = (
        modules = [(name = "worker", esModule = embed "worker.mjs")],
        compatibilityDate = "2025-06-01",
      )
    ),
    (name = "objects", disk = (writable = true)),
  ],
  sockets = [
    (name = "http", address = "127.0.0.1:0", http = (), service = "front"),
  ],
);
