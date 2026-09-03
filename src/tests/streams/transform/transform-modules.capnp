# Single source of truth for the transform streams suite's module set; both
# main cells reference this constant so the two implementations run exactly
# the same embedded code. embed paths resolve relative to THIS file.

@0xc7a2e94d1f8b6503;

using Workerd = import "/workerd/workerd.capnp";

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "api-surface", esModule = embed "api-surface.js"),
  (name = "transformer-algorithms", esModule = embed "transformer-algorithms.js"),
  (name = "error-propagation", esModule = embed "error-propagation.js"),
  (name = "backpressure", esModule = embed "backpressure.js"),
  (name = "reentrancy", esModule = embed "reentrancy.js"),
  (name = "roundtrip", esModule = embed "roundtrip.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "cancel-matrix", esModule = embed "cancel-matrix.js"),
  (name = "terminate", esModule = embed "terminate.js"),
  (name = "then-interceptors", esModule = embed "then-interceptors.js"),
  (name = "buffer-lifecycle", esModule = embed "buffer-lifecycle.js"),
  (name = "gc", esModule = embed "gc.js"),
  (name = "draining-reader", esModule = embed "draining-reader.js"),
  (name = "data-volumes", esModule = embed "data-volumes.js"),
];
