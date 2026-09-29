using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the scaling suite's test modules. Both
# cells (scaling-cpp.wd-test, scaling-ts.wd-test) reference this list so
# they always embed identical code.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "readable", esModule = embed "readable.js"),
  (name = "readable-byte", esModule = embed "readable-byte.js"),
  (name = "writable", esModule = embed "writable.js"),
  (name = "identity", esModule = embed "identity.js"),
];
