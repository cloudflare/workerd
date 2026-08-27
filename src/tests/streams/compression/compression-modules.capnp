# Single source of truth for the compression streams suite's module set;
# both main cells reference this constant so the two implementations run
# exactly the same embedded code. embed paths resolve relative to THIS file.

@0x9395ce7a94b15181;

using Workerd = import "/workerd/workerd.capnp";

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "api-surface", esModule = embed "api-surface.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "round-trip", esModule = embed "round-trip.js"),
  (name = "chunk-boundaries", esModule = embed "chunk-boundaries.js"),
  (name = "large-payload", esModule = embed "large-payload.js"),
  (name = "empty-stream", esModule = embed "empty-stream.js"),
  (name = "corrupt-input", esModule = embed "corrupt-input.js"),
  (name = "strict-checks", esModule = embed "strict-checks.js"),
  (name = "byob", esModule = embed "byob.js"),
  (name = "backpressure", esModule = embed "backpressure.js"),
  (name = "propagation", esModule = embed "propagation.js"),
  (name = "pipe-integration", esModule = embed "pipe-integration.js"),
  (name = "body-integration", esModule = embed "body-integration.js"),
  (name = "unhandled-rejection", esModule = embed "unhandled-rejection.js"),
  (name = "buffer-lifecycle", esModule = embed "buffer-lifecycle.js"),
];
