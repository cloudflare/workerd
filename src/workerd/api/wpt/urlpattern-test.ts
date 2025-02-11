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
  'urlpattern.any.js': {
    comment: 'these are invalid tests',
    expectedFailures: [],
  },
  'urlpattern.https.any.js': {
    comment: 'these are invalid tests',
    expectedFailures: [],
  },
  'urlpatterntests.js': {},
} satisfies TestRunnerConfig;
