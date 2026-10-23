using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the encoding stream benchmarks' modules.
# Both cells (encoding-cpp.wd-bench, encoding-ts.wd-bench) reference this
# list so they always embed identical code.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "encoder", esModule = embed "encoder.js"),
  (name = "decoder", esModule = embed "decoder.js"),
  (name = "pipeline", esModule = embed "pipeline.js"),
];
