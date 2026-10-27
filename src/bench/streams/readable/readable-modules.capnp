using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the readable benchmarks' modules. Both
# cells (readable-cpp.wd-bench, readable-ts.wd-bench) reference this list so
# they always embed identical code.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "reader", esModule = embed "reader.js"),
  (name = "read-loop", esModule = embed "read-loop.js"),
  (name = "async-iteration", esModule = embed "async-iteration.js"),
  (name = "tee", esModule = embed "tee.js"),
  (name = "from", esModule = embed "from.js"),
  (name = "cancel", esModule = embed "cancel.js"),
];
