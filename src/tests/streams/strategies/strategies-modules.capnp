# Single source of truth for the queuing strategies suite's module set;
# both main cells reference this constant so the two implementations run
# exactly the same embedded code. embed paths resolve relative to THIS file.

@0xa8435fa1ff6bc66f;

using Workerd = import "/workerd/workerd.capnp";

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "api-surface", esModule = embed "api-surface.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "size-semantics", esModule = embed "size-semantics.js"),
  (name = "integration", esModule = embed "integration.js"),
];
