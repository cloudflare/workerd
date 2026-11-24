# Single source of truth for the Blob suite's module set; every cell
# references it, so both implementations and every flag combination run
# exactly the same embedded code. embed paths resolve relative to THIS file.

@0xe4a1c7b0f2d93a65;

using Workerd = import "/workerd/workerd.capnp";

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "api-surface", esModule = embed "api-surface.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "slicing", esModule = embed "slicing.js"),
  (name = "reading", esModule = embed "reading.js"),
  (name = "serialization", esModule = embed "serialization.js"),
  (name = "consumers", esModule = embed "consumers.js"),
  (name = "inspect", esModule = embed "inspect.js"),
  (name = "gc", esModule = embed "gc.js"),
];
