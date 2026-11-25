# Single source of truth for the scheduler suite's module sets; every cell
# references them, so both implementations and every flag combination run
# exactly the same embedded code. embed paths resolve relative to THIS file.

@0xb8e2a41c9d3f6075;

using Workerd = import "/workerd/workerd.capnp";

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "shape", esModule = embed "shape.js"),
  (name = "wait", esModule = embed "wait.js"),
];

# A worker of its own for the first read of `scheduler` and the changes to
# the global that cannot be undone.
const firstReadModules :List(Workerd.Worker.Module) = [
  (name = "first-read", esModule = embed "first-read.js"),
];
