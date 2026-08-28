using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the cache suite's test modules.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "cache-streams", esModule = embed "cache-streams.js"),
];
