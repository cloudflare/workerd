// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

let addEventListenerStub: typeof globalThis.addEventListener;

export default {
  'reporterror.any.js': {
    // The harness replaces the global addEventListener with a no-op. This test listens for the
    // 'error' event that reportError() dispatches, so it needs the real EventTarget method.
    after(): void {
      globalThis.addEventListener = addEventListenerStub;
    },
    before(): void {
      addEventListenerStub = globalThis.addEventListener;
      // @ts-expect-error Removing the own property exposes the inherited EventTarget method
      delete globalThis.addEventListener;
    },
  },
} satisfies TestRunnerConfig;
