# Single source of truth for the writable streams suite's module set; both
# main cells reference this constant so the two implementations run exactly
# the same embedded code. embed paths resolve relative to THIS file.

@0xe4c8f1b2a9d07c31;

using Workerd = import "/workerd/workerd.capnp";

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "api-surface", esModule = embed "api-surface.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "sink-algorithms", esModule = embed "sink-algorithms.js"),
  (name = "write-semantics", esModule = embed "write-semantics.js"),
  (name = "close-semantics", esModule = embed "close-semantics.js"),
  (name = "abort-semantics", esModule = embed "abort-semantics.js"),
  (name = "abort-matrix", esModule = embed "abort-matrix.js"),
  (name = "backpressure", esModule = embed "backpressure.js"),
  (name = "buffer-lifecycle", esModule = embed "buffer-lifecycle.js"),
  (name = "reentrancy", esModule = embed "reentrancy.js"),
  (name = "then-interceptors", esModule = embed "then-interceptors.js"),
  (name = "gc", esModule = embed "gc.js"),
  (name = "data-volumes", esModule = embed "data-volumes.js"),
];
