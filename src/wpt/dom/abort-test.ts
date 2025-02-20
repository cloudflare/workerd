// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'AbortSignal.any.js': {},
  'abort-signal-any.any.js': {
    comment: 'Order of event firing should be investigated.',
    expectedFailures: [
      'Abort events for AbortSignal.any() signals fire in the right order (using AbortController)',
    ],
  },
  'event.any.js': {},
  'timeout-shadowrealm.any.js': {
    comment: 'Enable when ShadowRealm is implemented',
    skipAllTests: true,
  },
  'timeout.any.js': {},
} satisfies TestRunnerConfig;
