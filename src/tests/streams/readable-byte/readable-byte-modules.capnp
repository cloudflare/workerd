using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the readable-byte suite's test modules.
# All main cells reference this list so they always embed identical code.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "pull-timing", esModule = embed "pull-timing.js"),
  (name = "controller", esModule = embed "controller.js"),
  (name = "byob-reader", esModule = embed "byob-reader.js"),
  (name = "respond", esModule = embed "respond.js"),
  (name = "release-relock", esModule = embed "release-relock.js"),
  (name = "read-min", esModule = embed "read-min.js"),
];
