// Survey config.
import { type TestRunnerConfig } from 'harness/harness';

export default {
  "acquire.https.any.js": {},
  "bfcache/contention.https.window.js": {},
  "bfcache/helpers.js": {},
  "held.https.any.js": {},
  "idlharness.https.any.js": {},
  "ifAvailable.https.any.js": {},
  "lock-attributes.https.any.js": {},
  "mode-exclusive.https.any.js": {},
  "mode-mixed.https.any.js": {},
  "mode-shared.https.any.js": {},
  "non-secure-context.any.js": {},
  "query-empty.https.any.js": {},
  "query.https.any.js": {},
  "resource-names.https.any.js": {},
  "secure-context.https.any.js": {},
  "signal.https.any.js": {},
  "steal.https.any.js": {},
  "windows.https.window.js": {}
} satisfies TestRunnerConfig;
