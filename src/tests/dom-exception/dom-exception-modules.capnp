# Single source of truth for the DOMException suite's module set; every
# cell references it, so both implementations and every flag combination
# run exactly the same embedded code. embed paths resolve relative to THIS
# file.

@0xc7d24e91a35b8f06;

using Workerd = import "/workerd/workerd.capnp";

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "api-surface", esModule = embed "api-surface.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "errors", esModule = embed "errors.js"),
  (name = "stack", esModule = embed "stack.js"),
  (name = "enumeration", esModule = embed "enumeration.js"),
  (name = "native", esModule = embed "native.js"),
  (name = "serialization", esModule = embed "serialization.js"),
  (name = "tunneling", esModule = embed "tunneling.js"),
  (name = "inspect", esModule = embed "inspect.js"),
  (name = "quota-exceeded-error", esModule = embed "quota-exceeded-error.js"),
];
