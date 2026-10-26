# Single source of truth for the structuredClone suite's module set; both
# cells reference this constant so the two implementations run exactly the
# same embedded code. embed paths resolve relative to THIS file.

@0xc1f3d7a2b84e6905;

using Workerd = import "/workerd/workerd.capnp";

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "api-surface", esModule = embed "api-surface.js"),
  (name = "receiver", esModule = embed "receiver.js"),
  (name = "arguments", esModule = embed "arguments.js"),
  (name = "transfer-conversion", esModule = embed "transfer-conversion.js"),
  (name = "cloning", esModule = embed "cloning.js"),
  (name = "pollution", esModule = embed "pollution.js"),
];
