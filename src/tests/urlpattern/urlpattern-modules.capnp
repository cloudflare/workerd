# Single source of truth for the URLPattern suite's module set; every
# cell references it, so both implementations and every flag combination
# run exactly the same embedded code. embed paths resolve relative to THIS
# file.

@0xa3e81b6c4f2d9075;

using Workerd = import "/workerd/workerd.capnp";

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "api-surface", esModule = embed "api-surface.js"),
  (name = "illegal-invocation", esModule = embed "illegal-invocation.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "conversions", esModule = embed "conversions.js"),
  (name = "matching", esModule = embed "matching.js"),
  (name = "results", esModule = embed "results.js"),
  (name = "pollution", esModule = embed "pollution.js"),
  (name = "inspect", esModule = embed "inspect.js"),
  (name = "serialization", esModule = embed "serialization.js"),
];
