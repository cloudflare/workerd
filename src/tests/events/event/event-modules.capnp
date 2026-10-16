# Single source of truth for the Event suite's module set; every cell
# references this constant so the implementations run exactly the same
# embedded code. embed paths resolve relative to THIS file.

@0xf262a336ae1c6da3;

using Workerd = import "/workerd/workerd.capnp";

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "api-surface", esModule = embed "api-surface.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "cancelation", esModule = embed "cancelation.js"),
  (name = "propagation-flags", esModule = embed "propagation-flags.js"),
  (name = "init-event", esModule = embed "init-event.js"),
  (name = "custom-event", esModule = embed "custom-event.js"),
  (name = "subclassing", esModule = embed "subclassing.js"),
  (name = "pollution", esModule = embed "pollution.js"),
  (name = "interop", esModule = embed "interop.js"),
];
