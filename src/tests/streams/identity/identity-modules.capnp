# The single source of truth for the identity streams suite's module set.
# Both main cells — identity-cpp.wd-test and identity-ts.wd-test — reference
# the one constant below for their `modules` list, which guarantees the two
# implementations are exercised by exactly the same embedded code: the lists
# cannot drift apart because there is only one list. (The legacy cell runs a
# deliberately different, smaller module set and defines it inline in
# identity-cpp-legacy.wd-test.)
#
# embed paths resolve relative to THIS file.

@0xefde4626b220dd6f;

using Workerd = import "/workerd/workerd.capnp";

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "which-impl", esModule = embed "which-impl.js"),
  (name = "propagation-helpers", esModule = embed "propagation-helpers.js"),
  (name = "api-surface", esModule = embed "api-surface.js"),
  (name = "construction", esModule = embed "construction.js"),
  (name = "chunk-types", esModule = embed "chunk-types.js"),
  (name = "zero-length-writes", esModule = embed "zero-length-writes.js"),
  (name = "copy-semantics", esModule = embed "copy-semantics.js"),
  (name = "buffer-lifecycle", esModule = embed "buffer-lifecycle.js"),
  (name = "ordering", esModule = embed "ordering.js"),
  (name = "byob", esModule = embed "byob.js"),
  (name = "backpressure", esModule = embed "backpressure.js"),
  (name = "close-propagation", esModule = embed "close-propagation.js"),
  (name = "abort-propagation", esModule = embed "abort-propagation.js"),
  (name = "cancel-propagation", esModule = embed "cancel-propagation.js"),
  (name = "fixed-length", esModule = embed "fixed-length.js"),
  (name = "fixed-length-errors", esModule = embed "fixed-length-errors.js"),
  (name = "tee", esModule = embed "tee.js"),
  (name = "tee-backpressure", esModule = embed "tee-backpressure.js"),
  (name = "tee-nested", esModule = embed "tee-nested.js"),
  (name = "draining-reader", esModule = embed "draining-reader.js"),
  (name = "payload-helpers", esModule = embed "payload-helpers.js"),
  (name = "body-integration", esModule = embed "body-integration.js"),
  (name = "pipe-integration", esModule = embed "pipe-integration.js"),
  (name = "reentrancy", esModule = embed "reentrancy.js"),
  (name = "lock-release", esModule = embed "lock-release.js"),
  (name = "read-at-least", esModule = embed "read-at-least.js"),
  (name = "reader-writer-acquisition", esModule = embed "reader-writer-acquisition.js"),
  (name = "cancel-reason-types", esModule = embed "cancel-reason-types.js"),
  (name = "gc-interplay", esModule = embed "gc-interplay.js"),
  (name = "transfer", esModule = embed "transfer.js"),
];
