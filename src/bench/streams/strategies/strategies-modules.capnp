using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the queuing strategy benchmarks' modules.
# Both cells (strategies-cpp.wd-bench, strategies-ts.wd-bench) reference
# this list so they always embed identical code.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "strategy-objects", esModule = embed "strategy-objects.js"),
  (name = "queue-accounting", esModule = embed "queue-accounting.js"),
];
