# Single source of truth for the URL suite's module set; every cell
# references it, so both implementations and every flag combination run
# exactly the same embedded code. embed paths resolve relative to THIS file.

@0xc1f4a7d2e3b58a96;

using Workerd = import "/workerd/workerd.capnp";

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "api-surface", esModule = embed "api-surface.js"),
  (name = "illegal-invocation", esModule = embed "illegal-invocation.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "setters", esModule = embed "setters.js"),
  (name = "origin-cache", esModule = embed "origin-cache.js"),
  (name = "search-params-coupling", esModule = embed "search-params-coupling.js"),
  (name = "search-params", esModule = embed "search-params.js"),
  (name = "iteration", esModule = embed "iteration.js"),
  (name = "conversions", esModule = embed "conversions.js"),
  (name = "inspect", esModule = embed "inspect.js"),
  (name = "serialization", esModule = embed "serialization.js"),
  (name = "consumers", esModule = embed "consumers.js"),
  (name = "pollution", esModule = embed "pollution.js"),
  (name = "gc", esModule = embed "gc.js"),
  (name = "legacy-conformance", esModule = embed "legacy-conformance.js"),
  (name = "legacy-conformance-data", esModule = embed "legacy-conformance-data.js"),
];
