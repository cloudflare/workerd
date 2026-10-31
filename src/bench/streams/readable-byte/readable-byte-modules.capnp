using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the byte stream benchmarks' modules. Both
# cells (readable-byte-cpp.wd-bench, readable-byte-ts.wd-bench) reference
# this list so they always embed identical code.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "default-reader", esModule = embed "default-reader.js"),
  (name = "byob-reader", esModule = embed "byob-reader.js"),
  (name = "auto-allocate", esModule = embed "auto-allocate.js"),
  (name = "tee", esModule = embed "tee.js"),
];
