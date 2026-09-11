using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the node:stream suite's test modules.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "api-surface", esModule = embed "api-surface.js"),
  (name = "readable-to-web", esModule = embed "readable-to-web.js"),
  (name = "readable-from-web", esModule = embed "readable-from-web.js"),
  (name = "writable-to-web", esModule = embed "writable-to-web.js"),
  (name = "writable-from-web", esModule = embed "writable-from-web.js"),
  (name = "duplex-to-web", esModule = embed "duplex-to-web.js"),
  (name = "duplex-from-web", esModule = embed "duplex-from-web.js"),
  (name = "bodies", esModule = embed "bodies.js"),
];
