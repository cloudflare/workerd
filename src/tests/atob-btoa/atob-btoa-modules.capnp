# Single source of truth for the atob() and btoa() suite's module set; every
# cell references it, so both implementations and every flag combination
# run exactly the same embedded code. embed paths resolve relative to THIS
# file.

@0xb3f1a6c2d94e7805;

using Workerd = import "/workerd/workerd.capnp";

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "helpers", esModule = embed "helpers.js"),
  (name = "api-surface", esModule = embed "api-surface.js"),
  (name = "conversions", esModule = embed "conversions.js"),
  (name = "decoding", esModule = embed "decoding.js"),
  (name = "encoding", esModule = embed "encoding.js"),
  (name = "errors", esModule = embed "errors.js"),
  (name = "pollution", esModule = embed "pollution.js"),
];
