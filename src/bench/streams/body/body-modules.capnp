using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the body benchmarks' modules. Both cells
# (body-cpp.wd-bench, body-ts.wd-bench) reference this list so they always
# embed identical code.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "service-binding", esModule = embed "service-binding.js"),
  (name = "consume", esModule = embed "consume.js"),
  (name = "read", esModule = embed "read.js"),
  (name = "pipe", esModule = embed "pipe.js"),
  (name = "clone", esModule = embed "clone.js"),
  (name = "html-rewriter", esModule = embed "html-rewriter.js"),
  (name = "form-data", esModule = embed "form-data.js"),
];
