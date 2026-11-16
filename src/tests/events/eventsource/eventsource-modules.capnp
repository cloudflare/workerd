# Single source of truth for the EventSource suite's module sets and the
# services every main and opt-in cell shares; every cell references these
# constants, so the two implementations run exactly the same embedded code.
# embed paths resolve relative to THIS file.

@0xb7d4e3f1a2c59e61;

using Workerd = import "/workerd/workerd.capnp";

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "server", esModule = embed "server.js"),
  (name = "api-surface", esModule = embed "api-surface.js"),
  (name = "inspect", esModule = embed "inspect.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "illegal-invocation", esModule = embed "illegal-invocation.js"),
  (name = "from", esModule = embed "from.js"),
  (name = "parser", esModule = embed "parser.js"),
  (name = "request", esModule = embed "request.js"),
  (name = "response", esModule = embed "response.js"),
  (name = "reconnection", esModule = embed "reconnection.js"),
  (name = "event-shapes", esModule = embed "event-shapes.js"),
  (name = "handlers", esModule = embed "handlers.js"),
  (name = "close", esModule = embed "close.js"),
  (name = "listener-exceptions", esModule = embed "listener-exceptions.js"),
  (name = "context", esModule = embed "context.js"),
  (name = "gc", esModule = embed "gc.js"),
  (name = "pollution", esModule = embed "pollution.js"),
];

# The legacy cells' set. Shared: both implementations honor the flags these
# cells leave off.
const legacyModules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "legacy-main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "legacy-shape", esModule = embed "legacy-shape.js"),
  (name = "legacy-listener-exceptions", esModule = embed "legacy-listener-exceptions.js"),
];

# context.js's fixture: the same worker with and without
# handle_cross_request_promise_resolution, fetching from a helper.
const contextCurrent :Workerd.Service = (
  name = "context-current",
  worker = (
    modules = [(name = "worker", esModule = embed "context-worker.js")],
    compatibilityFlags = ["handle_cross_request_promise_resolution"],
    bindings = [(name = "backend", service = "context-helper")],
  ),
);

const contextLegacy :Workerd.Service = (
  name = "context-legacy",
  worker = (
    modules = [(name = "worker", esModule = embed "context-worker.js")],
    compatibilityFlags = ["no_handle_cross_request_promise_resolution"],
    bindings = [(name = "backend", service = "context-helper")],
  ),
);

const contextHelper :Workerd.Service = (
  name = "context-helper",
  worker = (
    modules = [(name = "worker", esModule = embed "context-worker.js")],
  ),
);
