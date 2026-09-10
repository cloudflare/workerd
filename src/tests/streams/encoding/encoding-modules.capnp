# Single source of truth for the encoding streams suite's module set; both
# main cells reference this constant so the two implementations run exactly
# the same embedded code. embed paths resolve relative to THIS file.

@0xb5f47130b8e48ee2;

using Workerd = import "/workerd/workerd.capnp";

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "api-surface", esModule = embed "api-surface.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "encode-coercion", esModule = embed "encode-coercion.js"),
  (name = "chunk-types", esModule = embed "chunk-types.js"),
  (name = "zero-length-writes", esModule = embed "zero-length-writes.js"),
  (name = "encode-surrogates", esModule = embed "encode-surrogates.js"),
  (name = "decode-splits", esModule = embed "decode-splits.js"),
  (name = "fatal-mode", esModule = embed "fatal-mode.js"),
  (name = "backpressure", esModule = embed "backpressure.js"),
  (name = "propagation", esModule = embed "propagation.js"),
  (name = "tee", esModule = embed "tee.js"),
  (name = "body-integration", esModule = embed "body-integration.js"),
  (name = "buffer-lifecycle", esModule = embed "buffer-lifecycle.js"),
  (name = "reentrancy", esModule = embed "reentrancy.js"),
  (name = "draining-reader", esModule = embed "draining-reader.js"),
  (name = "gc-interplay", esModule = embed "gc-interplay.js"),
  (name = "decode-non-utf8", esModule = embed "decode-non-utf8.js"),
  (name = "pipe-integration", esModule = embed "pipe-integration.js"),
];
