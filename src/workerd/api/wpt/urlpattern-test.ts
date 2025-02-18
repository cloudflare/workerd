// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'wpt:harness';

export default {
  'urlpattern-compare-tests.tentative.js': {
    comment: 'URLPattern.compareComponent is not part of the URLPattern spec',
    skipAllTests: true,
  },
  'urlpattern-compare.tentative.any.js': {
    comment: 'URLPattern.compareComponent is not part of the URLPattern spec',
    skipAllTests: true,
  },
  'urlpattern-compare.tentative.https.any.js': {
    comment: 'URLPattern.compareComponent is not part of the URLPattern spec',
    skipAllTests: true,
  },
  'urlpattern-hasregexpgroups-tests.js': {},
  'urlpattern-hasregexpgroups.any.js': {},
  'urlpattern.any.js': {},
  'urlpattern.https.any.js': {},
  'urlpatterntests.js': {
    comment: 'skipping invalid tests',
    expectedFailures: [
      'Pattern: [{"protocol":"http","port":"80 "}] Inputs: [{"protocol":"http","port":"80"}]',
      'Pattern: ["https://{sub.}?example{.com/}foo"] Inputs: ["https://example.com/foo"]',
      'Pattern: [{"hostname":"bad#hostname"}] Inputs: undefined',
      'Pattern: [{"hostname":"bad/hostname"}] Inputs: undefined',
      'Pattern: [{"hostname":"bad\\\\\\\\hostname"}] Inputs: undefined',
      'Pattern: [{"hostname":"bad\\nhostname"}] Inputs: undefined',
      'Pattern: [{"hostname":"bad\\rhostname"}] Inputs: undefined',
      'Pattern: [{"hostname":"bad\\thostname"}] Inputs: undefined',
    ],
  },
} satisfies TestRunnerConfig;
