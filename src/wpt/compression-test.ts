// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'compression-bad-chunks.any.js': {
    comment: 'Test times out - needs investigation',
    disabledTests: true,
  },
  'compression-constructor-error.any.js': {},
  'compression-including-empty-chunk.any.js': {
    comment: 'brotli compression is not supported',
    expectedFailures: [
      "the result of compressing [,Hello,Hello] with brotli should be 'HelloHello'",
      "the result of compressing [Hello,,Hello] with brotli should be 'HelloHello'",
      "the result of compressing [Hello,Hello,] with brotli should be 'HelloHello'",
    ],
  },
  'compression-large-flush-output.any.js': {
    comment: 'brotli compression is not supported',
    expectedFailures: ['brotli compression with large flush output'],
  },
  'compression-multiple-chunks.any.js': {
    comment: 'brotli compression is not supported',
    expectedFailures: [/compressing \d+ chunks with brotli should work/],
  },
  'compression-output-length.any.js': {
    comment: 'brotli compression is not supported',
    expectedFailures: [
      'the length of brotli data should be shorter than that of the original data',
    ],
  },
  'compression-stream.any.js': {
    comment: 'brotli compression is not supported',
    expectedFailures: [
      /brotli .* data should be reinflated back to its origin/,
    ],
  },
  'compression-with-detach.window.js': {},
  'decompression-bad-chunks.any.js': {
    comment: 'brotli compression is not supported',
    expectedFailures: [/brotli/],
  },
  'decompression-buffersource.any.js': {
    comment: 'brotli compression is not supported',
    expectedFailures: [/brotli/],
  },
  'decompression-constructor-error.any.js': {
    comment:
      'brotli compression is not supported - these pass because brotli throws',
  },
  'decompression-correct-input.any.js': {
    comment: 'brotli compression is not supported',
    expectedFailures: [/.*brotli.*/],
  },
  'decompression-corrupt-input.any.js': {
    comment: 'brotli compression is not supported',
    expectedFailures: [/brotli/],
  },
  'decompression-empty-input.any.js': {
    comment: 'brotli compression is not supported',
    expectedFailures: [/.*brotli.*/],
  },
  'decompression-extra-input.any.js': {
    comment:
      'Extra padding tests fail - workerd handles trailing data differently',
    expectedFailures: [
      'decompressing deflate input with extra pad should still give the output',
      'decompressing gzip input with extra pad should still give the output',
      'decompressing deflate-raw input with extra pad should still give the output',
      /brotli/,
    ],
  },
  'decompression-split-chunk.any.js': {
    comment: 'brotli compression is not supported',
    expectedFailures: [/.*brotli/],
  },
  'decompression-uint8array-output.any.js': {
    comment: 'brotli compression is not supported',
    expectedFailures: [
      'decompressing brotli output should give Uint8Array chunks',
    ],
  },
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
