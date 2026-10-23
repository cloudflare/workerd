using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the compression stream benchmarks' modules.
# Both cells (compression-cpp.wd-bench, compression-ts.wd-bench) reference
# this list so they always embed identical code.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "compress", esModule = embed "compress.js"),
  (name = "decompress", esModule = embed "decompress.js"),
  (name = "pipeline", esModule = embed "pipeline.js"),
];
