using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the node:net suite's test modules.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "servers", esModule = embed "servers.js"),
  (name = "connect-lifecycle", esModule = embed "connect-lifecycle.js"),
  (name = "echo-roundtrip", esModule = embed "echo-roundtrip.js"),
  (name = "half-close", esModule = embed "half-close.js"),
  (name = "end-and-destroy", esModule = embed "end-and-destroy.js"),
  (name = "backpressure", esModule = embed "backpressure.js"),
  (name = "timeouts", esModule = embed "timeouts.js"),
  (name = "onread", esModule = embed "onread.js"),
  (name = "interop", esModule = embed "interop.js"),
  (name = "data-volumes", esModule = embed "data-volumes.js"),
];
