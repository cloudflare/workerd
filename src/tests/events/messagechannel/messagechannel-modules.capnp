# Single source of truth for the MessageChannel suite's module sets; every
# cell references one of these constants, so the two implementations run
# exactly the same embedded code. embed paths resolve relative to THIS file.

@0xff44fb175392e5f8;

using Workerd = import "/workerd/workerd.capnp";

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "api-surface", esModule = embed "api-surface.js"),
  (name = "inspect", esModule = embed "inspect.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "illegal-invocation", esModule = embed "illegal-invocation.js"),
  (name = "post-message-arguments", esModule = embed "post-message-arguments.js"),
  (name = "cloning", esModule = embed "cloning.js"),
  (name = "delivery", esModule = embed "delivery.js"),
  (name = "message-event", esModule = embed "message-event.js"),
  (name = "onmessage", esModule = embed "onmessage.js"),
  (name = "port-queue", esModule = embed "port-queue.js"),
  (name = "close", esModule = embed "close.js"),
  (name = "listener-exceptions", esModule = embed "listener-exceptions.js"),
  (name = "gc", esModule = embed "gc.js"),
  (name = "pollution", esModule = embed "pollution.js"),
];

# The unflagged legacy cells' set. Unlike the streams suites, it is shared:
# both implementations honor the flags these cells leave off.
const legacyModules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "legacy-main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "legacy-shape", esModule = embed "legacy-shape.js"),
  (name = "legacy-listener-exceptions", esModule = embed "legacy-listener-exceptions.js"),
];
