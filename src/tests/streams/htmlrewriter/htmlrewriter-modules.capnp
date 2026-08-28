using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the htmlrewriter suite's test modules.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "rewrite-streams", esModule = embed "rewrite-streams.js"),
];
