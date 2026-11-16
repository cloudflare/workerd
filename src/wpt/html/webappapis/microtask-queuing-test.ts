// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'queue-microtask-exceptions.any.js': {
    comment:
      'The test listens for the global error event, but the harness replaces globalThis.addEventListener with a no-op stub (src/wpt/harness/globals.ts), so the listener is never registered and the test hangs',
    disabledTests: ['It rethrows exceptions'],
  },
  'queue-microtask.any.js': {},
  'queue-microtask.window.js': {
    comment:
      'Both subtests use MutationObserver and document.createTextNode; workerd has no DOM',
    omittedTests: [
      'It interleaves with MutationObservers as expected',
      'It interleaves with MutationObservers and promises together as expected',
    ],
  },
} satisfies TestRunnerConfig;
