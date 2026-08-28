using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the iocontext suite's test modules.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "global-scope-streams", esModule = embed "global-scope-streams.js"),
];
