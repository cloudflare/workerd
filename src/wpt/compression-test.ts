// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'compression-large-flush-output.any.js': {},
  'idlharness.https.any.js': {
    comment:
      'Workers expose globals differently than browsers - these interface tests fail',
    expectedFailures: [
      'CompressionStream interface: existence and properties of interface object',
      'CompressionStream interface object length',
      'CompressionStream interface object name',
      'CompressionStream interface: existence and properties of interface prototype object',
      'CompressionStream interface: existence and properties of interface prototype object\'s "constructor" property',
      "CompressionStream interface: existence and properties of interface prototype object's @@unscopables property",
      'CompressionStream must be primary interface of new CompressionStream("deflate")',
      'Stringification of new CompressionStream("deflate")',
      'DecompressionStream interface: existence and properties of interface object',
      'DecompressionStream interface object length',
      'DecompressionStream interface object name',
      'DecompressionStream interface: existence and properties of interface prototype object',
      'DecompressionStream interface: existence and properties of interface prototype object\'s "constructor" property',
      "DecompressionStream interface: existence and properties of interface prototype object's @@unscopables property",
      'DecompressionStream must be primary interface of new DecompressionStream("deflate")',
      'Stringification of new DecompressionStream("deflate")',
    ],
  },
  'third_party/pako/pako_inflate.min.js': {},
} satisfies TestRunnerConfig;
