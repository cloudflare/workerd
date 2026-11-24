# Single source of truth for the Cloudflare global suite's module sets; every
# cell references them, so both implementations and every flag combination
# run exactly the same embedded code. embed paths resolve relative to THIS
# file.

@0xd3c1f5a2b7e94c68;

using Workerd = import "/workerd/workerd.capnp";

# The suite. The schema that defines the compatibility flags is a text
# module, from which the tests take the names the flags object must list.
const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "shape", esModule = embed "shape.js"),
  (name = "flags", esModule = embed "flags.js"),
  (name = "compatibility-date.capnp", text = embed "../../workerd/io/compatibility-date.capnp"),
];

# A worker of its own for the first read of `Cloudflare` and the changes to
# the object that cannot be undone.
const firstReadModules :List(Workerd.Worker.Module) = [
  (name = "first-read", esModule = embed "first-read.js"),
  (name = "compatibility-date.capnp", text = embed "../../workerd/io/compatibility-date.capnp"),
];
