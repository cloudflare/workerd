// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'compression-bad-chunks.any.js': {},
  'compression-constructor-error.any.js': {},
  'compression-including-empty-chunk.any.js': {},
  'compression-large-flush-output.any.js': {},
  'compression-multiple-chunks.any.js': {},
  'compression-output-length.any.js': {},
  'compression-stream.any.js': {},
  'compression-with-detach.window.js': {},
  'decompression-bad-chunks.any.js': {},
  'decompression-buffersource.any.js': {},
  'decompression-constructor-error.any.js': {},
  'decompression-correct-input.any.js': {},
  'decompression-corrupt-input.any.js': {},
  'decompression-empty-input.any.js': {},
  'decompression-extra-input.any.js': {
    comment:
      'Extra padding tests fail - workerd handles trailing data differently',
    expectedFailures: [
      'decompressing deflate input with extra pad should still give the output',
      'decompressing gzip input with extra pad should still give the output',
      'decompressing deflate-raw input with extra pad should still give the output',
    ],
  },
  'decompression-split-chunk.any.js': {},
  'decompression-uint8array-output.any.js': {},
  'decompression-with-detach.window.js': {
    comment: 'Detach test fails - needs investigation',
    expectedFailures: [
      'data should be correctly decompressed even if input is detached partway',
    ],
  },
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
