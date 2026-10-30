using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the writable benchmarks' modules. Both
# cells (writable-cpp.wd-bench, writable-ts.wd-bench) reference this list so
# they always embed identical code.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "writer", esModule = embed "writer.js"),
  (name = "write-loop", esModule = embed "write-loop.js"),
  (name = "abort", esModule = embed "abort.js"),
];
