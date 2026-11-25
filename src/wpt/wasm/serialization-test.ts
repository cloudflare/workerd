// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'arraybuffer/transfer.window.js': {
    comment:
      'Calls window.postMessage(message, targetOrigin), which only exists on a Window',
    expectedFailures: [
      'Serializing a WebAssembly.Memory-backed ArrayBuffer works',
      'Transfering a WebAssembly.Memory-backed ArrayBuffer throws',
    ],
  },
  'memory/clone-error.tentative.https.any.js': {
    comment:
      'MessagePort.postMessage() rejects a shared WebAssembly.Memory with a TypeError instead of a DataCloneError; BroadcastChannel and self.crossOriginIsolated are not implemented in workerd',
    expectedFailures: [
      'Cloning a shared WebAssembly.Memory through a MessagePort without COOP+COEP throws',
      'Cloning a shared WebAssembly.Memory through a BroadcastChannel without COOP+COEP throws',
      'Bonus: self.crossOriginIsolated is false (precondition for the shared-memory cases above)',
    ],
  },
  'module/nested-worker-success.any.js': {
    comment: 'Worker is not implemented in workerd',
    expectedFailures: [
      "postMessaging to a dedicated sub-worker allows them to see each others' modifications",
    ],
  },
  'module/serialization-via-idb.any.js': {
    comment:
      'IndexedDB is not implemented in workerd, and the test loads /IndexedDB/resources/support.js, which is outside this WPT directory',
    disabledTests: true,
  },
  'module/serialization-via-notifications-api.any.js': {
    comment: 'Notification is not implemented in workerd',
    expectedFailures: [
      "WebAssembly.Module cloning via the Notifications API's data member: basic case",
      "WebAssembly.Module cloning via the Notifications API's data member: is interleaved correctly",
    ],
  },
} satisfies TestRunnerConfig;
