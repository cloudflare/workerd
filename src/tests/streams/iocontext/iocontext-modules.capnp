using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the iocontext suite's test modules.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "global-scope-streams", esModule = embed "global-scope-streams.js"),
  (name = "global-scope-body-reads", esModule = embed "global-scope-body-reads.js"),
  (name = "global-scope-body-limit", esModule = embed "global-scope-body-limit.js"),
];
