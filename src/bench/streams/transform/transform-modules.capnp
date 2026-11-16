using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the transform benchmarks' modules. Both
# cells (transform-cpp.wd-bench, transform-ts.wd-bench) reference this list
# so they always embed identical code.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "roundtrip", esModule = embed "roundtrip.js"),
  (name = "throughput", esModule = embed "throughput.js"),
];
