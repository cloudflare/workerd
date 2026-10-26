using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the identity stream benchmarks' modules.
# Both cells (identity-cpp.wd-bench, identity-ts.wd-bench) reference this
# list so they always embed identical code.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "roundtrip", esModule = embed "roundtrip.js"),
  (name = "throughput", esModule = embed "throughput.js"),
  (name = "pipe", esModule = embed "pipe.js"),
];
