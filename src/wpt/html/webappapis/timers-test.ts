// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'clearinterval-from-callback.any.js': {},
  'cleartimeout-clearinterval.any.js': {},
  'evil-spec-example.any.js': {
    comment:
      'Passes a non-function handler, which WebIDL stringifies and evaluates as code. workerd does not support string handlers because it disallows eval; setTimeout throws a TypeError at the top level.',
    omittedTests: true,
  },
  'missing-timeout-setinterval.any.js': {},
  'negative-setinterval.any.js': {},
  'negative-settimeout.any.js': {},
  'setinterval-settimeout-clamping.any.js': {},
  'type-long-setinterval.any.js': {
    comment:
      'Hangs: workerd takes the setInterval delay as a double instead of converting it to a WebIDL long, so a delay of 2 ** 32 waits about 49 days instead of wrapping to 0. done() is never called, so the single test never completes.',
    disabledTests: true,
  },
  'type-long-settimeout.any.js': {
    comment:
      'Hangs: workerd takes the setTimeout delay as a double instead of converting it to a WebIDL long, so a delay of 2 ** 32 waits about 49 days instead of wrapping to 0. done() is never called, so the single test never completes.',
    disabledTests: true,
  },
  'zero-settimeout.any.js': {
    comment:
      'Assumes setTimeout(f, 1) and the later setTimeout(g, 0) are called less than 1ms apart. workerd measures each delay from the precise wall clock at the call, so on a slow build the 1ms deadline can come first and fire first, which the HTML spec permits: it orders only an earlier timer whose delay is no greater than the later one. workerd does not clamp a 0ms delay to 1ms (see IoContext::setTimeoutImpl()).',
    omittedTests: ['A 0ms timeout should not be clamped to 1ms'],
  },
} satisfies TestRunnerConfig;
