// Survey config.
import { type TestRunnerConfig } from 'harness/harness';

export default {
  "buckets/idlharness-worker.https.any.js": {},
  "buckets/opaque-origin.https.window.js": {},
  "estimate-indexeddb.https.any.js": {},
  "estimate-parallel.https.any.js": {},
  "helpers.js": {},
  "idlharness.https.any.js": {},
  "opaque-origin.https.window.js": {},
  "permission-query.https.any.js": {},
  "persisted.https.any.js": {},
  "storagemanager-estimate.https.any.js": {},
  "storagemanager-persist-persisted-match.https.window.js": {},
  "storagemanager-persist.https.window.js": {},
  "storagemanager-persist.https.worker.js": {},
  "storagemanager-persisted.https.any.js": {}
} satisfies TestRunnerConfig;
