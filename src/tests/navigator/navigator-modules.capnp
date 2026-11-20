# Single source of truth for the navigator suite's module sets; every cell
# references them, so both implementations and every flag combination run
# exactly the same embedded code. embed paths resolve relative to THIS file.

@0xe2a9c41f7d5b8306;

using Workerd = import "/workerd/workerd.capnp";

# The suite.
const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "top-level", esModule = embed "top-level.js"),
  (name = "api-surface", esModule = embed "api-surface.js"),
  (name = "attributes", esModule = embed "attributes.js"),
  (name = "send-beacon", esModule = embed "send-beacon.js"),
  (name = "pollution", esModule = embed "pollution.js"),
];

# A worker of its own for the first read of `navigator` and the changes to
# the instance that cannot be undone.
const firstReadModules :List(Workerd.Worker.Module) = [
  (name = "first-read", esModule = embed "first-read.js"),
];

# The service the suite's beacons are sent to.
const sinkModules :List(Workerd.Worker.Module) = [
  (name = "sink", esModule = embed "sink.js"),
];

# Without global_navigator.
const absentModules :List(Workerd.Worker.Module) = [
  (name = "absent", esModule = embed "absent.js"),
];
