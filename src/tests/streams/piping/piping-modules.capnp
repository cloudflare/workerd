using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the piping suite's test modules. All
# main cells reference this list so they always embed identical code.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "pipe-matrix", esModule = embed "pipe-matrix.js"),
  (name = "api-surface", esModule = embed "api-surface.js"),
  (name = "error-propagation", esModule = embed "error-propagation.js"),
  (name = "close-propagation", esModule = embed "close-propagation.js"),
  (name = "flow-control", esModule = embed "flow-control.js"),
  (name = "interop", esModule = embed "interop.js"),
  (name = "data-volumes", esModule = embed "data-volumes.js"),
  (name = "special-buffers", esModule = embed "special-buffers.js"),
];
