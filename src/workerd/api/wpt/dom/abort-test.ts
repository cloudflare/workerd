// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'wpt:harness';

export default {
  'AbortSignal.any.js': {},
  'abort-signal-any-tests.js': {},
  'abort-signal-any.any.js': {
    comment:
      '(1, 2) Target should be set to signal. (3) Should be investigated.',
    expectedFailures: [
      'AbortSignal.any() follows a single signal (using AbortController)',
      'AbortSignal.any() follows multiple signals (using AbortController)',
      'Abort events for AbortSignal.any() signals fire in the right order (using AbortController)',
    ],
    includeFile: 'abort-signal-any-tests.js',
  },
  'event.any.js': {
    comment: 'Target should be set to signal',
    expectedFailures: ['the abort event should have the right properties'],
  },
  'timeout-shadowrealm.any.js': {
    comment: 'Enable when ShadowRealm is implemented',
    skipAllTests: true,
  },
  'timeout.any.js': {},
} satisfies TestRunnerConfig;
