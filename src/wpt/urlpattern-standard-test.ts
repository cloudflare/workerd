// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'urlpattern-compare.tentative.any.js': {
    comment: 'URLPattern.compareComponent is not part of the URLPattern spec',
    skipAllTests: true,
  },
  'urlpattern-compare.tentative.https.any.js': {
    comment: 'URLPattern.compareComponent is not part of the URLPattern spec',
    skipAllTests: true,
  },
  'urlpattern-hasregexpgroups.any.js': {},
  'urlpattern.any.js': {
    comment: 'Invalid tests',
    expectedFailures: [
      'Pattern: ["https://{sub.}?example{.com/}foo"] Inputs: ["https://example.com/foo"]',
      'Pattern: [{"pathname":"\\ud83d \\udeb2"}] Inputs: []',
    ],
  },
  'urlpattern.https.any.js': {},
} satisfies TestRunnerConfig;
