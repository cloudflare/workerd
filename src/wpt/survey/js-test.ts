// Survey config.
import { type TestRunnerConfig } from 'harness/harness';

export default {
  "behaviours/HostEnsureCanAddPrivateElement.window.js": {},
  "builtins/Math.maxmin.js": {},
  "builtins/weakrefs/cleanup-prevented-with-reference.optional.any.js": {},
  "builtins/weakrefs/cleanup-prevented-with-unregister.optional.any.js": {},
  "builtins/weakrefs/finalizationregistry-cleanupCallback-gets-a-microtask.optional.any.js": {},
  "builtins/weakrefs/finalizationregistry-cleanupCallback-throws-onerror-interaction.optional.window.js": {},
  "builtins/weakrefs/gc-has-one-chance-to-call-cleanupCallback-queueMicrotask.optional.any.js": {},
  "builtins/weakrefs/gc-has-one-chance-to-call-cleanupCallback-queueMicrotaskMutationObserver.optional.window.js": {},
  "builtins/weakrefs/gc-has-one-chance-to-call-cleanupCallback.optional.any.js": {},
  "builtins/weakrefs/holdings-multiple-values.optional.any.js": {},
  "builtins/weakrefs/reentrancy.optional.any.js": {},
  "builtins/weakrefs/return-undefined-with-gc.optional.any.js": {},
  "builtins/weakrefs/unregister-cleaned-up-cell.optional.any.js": {}
} satisfies TestRunnerConfig;
