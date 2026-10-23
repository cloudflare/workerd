using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the DigestStream benchmarks' modules. Both
# cells (digest-cpp.wd-bench, digest-ts.wd-bench) reference this list so
# they always embed identical code.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "write", esModule = embed "write.js"),
  (name = "pipe", esModule = embed "pipe.js"),
];
