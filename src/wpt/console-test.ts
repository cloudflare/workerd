// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'console-is-a-namespace.any.js': {},
  'console-label-conversion.any.js': {},
  'console-log-large-array.any.js': {},
  'console-log-symbol.any.js': {},
  'console-namespace-object-class-string.any.js': {},
  'console-tests-historical.any.js': {},
  'helper.js': {
    comment: 'Helper file picked up by discovery, not a test',
    omittedTests: true,
  },
  'idlharness.any.js': {},
} satisfies TestRunnerConfig;
