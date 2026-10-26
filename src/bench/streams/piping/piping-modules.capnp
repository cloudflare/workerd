using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the piping benchmarks' modules. Both cells
# (piping-cpp.wd-bench, piping-ts.wd-bench) reference this list so they
# always embed identical code.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "pipe-to", esModule = embed "pipe-to.js"),
  (name = "pipe-through", esModule = embed "pipe-through.js"),
  (name = "pipe-options", esModule = embed "pipe-options.js"),
  (name = "byte-pipes", esModule = embed "byte-pipes.js"),
];
