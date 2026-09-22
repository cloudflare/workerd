# Single source of truth for the digest streams suite's module set; both
# main cells reference this constant so the two implementations run exactly
# the same embedded code. embed paths resolve relative to THIS file.

@0xd743a0c033ba9bfe;

using Workerd = import "/workerd/workerd.capnp";

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "api-surface", esModule = embed "api-surface.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "digest-vectors", esModule = embed "digest-vectors.js"),
  (name = "string-encoding", esModule = embed "string-encoding.js"),
  (name = "chunk-types", esModule = embed "chunk-types.js"),
  (name = "digest-promise", esModule = embed "digest-promise.js"),
  (name = "lifecycle", esModule = embed "lifecycle.js"),
  (name = "dispose", esModule = embed "dispose.js"),
  (name = "unhandled-rejection", esModule = embed "unhandled-rejection.js"),
  (name = "buffer-lifecycle", esModule = embed "buffer-lifecycle.js"),
  (name = "pipe-integration", esModule = embed "pipe-integration.js"),
  (name = "large-payload", esModule = embed "large-payload.js"),
  (name = "gc-interplay", esModule = embed "gc-interplay.js"),
  (name = "reentrancy", esModule = embed "reentrancy.js"),
  (name = "backpressure", esModule = embed "backpressure.js"),
];
