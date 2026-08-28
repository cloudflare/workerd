using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the piping suite's test modules. All
# main cells reference this list so they always embed identical code.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "pipe-matrix", esModule = embed "pipe-matrix.js"),
  (name = "api-surface", esModule = embed "api-surface.js"),
  (name = "error-propagation", esModule = embed "error-propagation.js"),
];
