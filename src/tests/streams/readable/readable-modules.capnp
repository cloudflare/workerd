using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the readable suite's test modules. Both
# main cells (readable-cpp.wd-test, readable-ts.wd-test) and the pedantic
# cell reference this list so they always embed identical code.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "api-surface", esModule = embed "api-surface.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "source-algorithms", esModule = embed "source-algorithms.js"),
  (name = "controller", esModule = embed "controller.js"),
  (name = "reader", esModule = embed "reader.js"),
  (name = "cancel", esModule = embed "cancel.js"),
  (name = "bad-strategies", esModule = embed "bad-strategies.js"),
  (name = "queue-math", esModule = embed "queue-math.js"),
  (name = "tee", esModule = embed "tee.js"),
  (name = "tee-reentrancy", esModule = embed "tee-reentrancy.js"),
  (name = "from", esModule = embed "from.js"),
  (name = "async-iteration", esModule = embed "async-iteration.js"),
  (name = "reentrancy", esModule = embed "reentrancy.js"),
  (name = "buffer-lifecycle", esModule = embed "buffer-lifecycle.js"),
  (name = "integration-body", esModule = embed "integration-body.js"),
  (name = "integration-locked-disturbed", esModule = embed "integration-locked-disturbed.js"),
  (name = "gc", esModule = embed "gc.js"),
  (name = "then-interceptors", esModule = embed "then-interceptors.js"),
  (name = "draining-reader", esModule = embed "draining-reader.js"),
  (name = "data-volumes", esModule = embed "data-volumes.js"),
];
