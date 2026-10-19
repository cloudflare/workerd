# Single source of truth for the EventTarget suite's module set; every cell
# references this constant so the implementations run exactly the same
# embedded code. embed paths resolve relative to THIS file.

@0x95497f81d2eb304d;

using Workerd = import "/workerd/workerd.capnp";

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "api-surface", esModule = embed "api-surface.js"),
  (name = "listeners", esModule = embed "listeners.js"),
  (name = "options", esModule = embed "options.js"),
  (name = "dispatch", esModule = embed "dispatch.js"),
  (name = "handle-event", esModule = embed "handle-event.js"),
  (name = "signal", esModule = embed "signal.js"),
  (name = "exceptions", esModule = embed "exceptions.js"),
  (name = "reflection", esModule = embed "reflection.js"),
  (name = "interop", esModule = embed "interop.js"),
  (name = "subclassing", esModule = embed "subclassing.js"),
  (name = "pollution", esModule = embed "pollution.js"),
  (name = "gc", esModule = embed "gc.js"),
];
