using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the r2-patterns suite's test modules.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "r2-consumption", esModule = embed "r2-consumption.js"),
];
