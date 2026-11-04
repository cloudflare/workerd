// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'basic.any.js': {},
  'idlharness.any.js': {
    comment:
      'The Performance interface object does not have EventTarget as its [[Prototype]], even though performance instances inherit from EventTarget.prototype. performance is a lazy instance property of the global scope, so it has no getter.',
    expectedFailures: [
      'Performance interface: existence and properties of interface object',
      'Window interface: attribute performance',
    ],
  },
  'monotonic-clock.any.js': {},
  'window-worker-timeOrigin.window.js': {
    comment:
      'Window-only test that compares timeOrigin with a dedicated Worker created from a blob: URL; workerd has neither Worker nor URL.createObjectURL',
    omittedTests: true,
  },
} satisfies TestRunnerConfig;
