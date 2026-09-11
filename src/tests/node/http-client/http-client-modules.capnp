using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the node:http client suite's test modules.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "harness", esModule = embed "harness.js"),
  (name = "response-body", esModule = embed "response-body.js"),
  (name = "request-body", esModule = embed "request-body.js"),
  (name = "lifecycle", esModule = embed "lifecycle.js"),
  (name = "interop", esModule = embed "interop.js"),
];
