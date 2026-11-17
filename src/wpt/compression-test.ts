// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'compression-bad-chunks.any.js': {
    comment:
      'The C++ implementation accepts string and SharedArrayBuffer chunks ' +
      '(src/tests/streams/compression/AGENTS.md ledger #1-#2), so the ' +
      'expected rejections never arrive and the test times out',
    disabledTests: true,
  },
  'compression-constructor-error.any.js': {},
  'compression-including-empty-chunk.any.js': {},
  'compression-large-flush-output.any.js': {},
  'compression-multiple-chunks.any.js': {},
  'compression-output-length.any.js': {},
  'compression-stream.any.js': {},
  'compression-with-detach.any.js': {},
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
      'decompressing brotli input with extra pad should still give the output',
    ],
  },
  'decompression-split-chunk.any.js': {},
  'decompression-uint8array-output.any.js': {},
  'decompression-with-detach.any.js': {
    comment:
      'Environmental, not a streams defect: compression-with-detach.any.js runs first in ' +
      'the same isolate and installs its Object.prototype.then trap without configurable, ' +
      'so this test\'s identical defineProperty throws "Cannot redefine property". Browsers ' +
      'give each .any.js file a fresh global; the shared-isolate harness cannot (the ' +
      'leftover is non-configurable, so it cannot even be deleted between files).',
    expectedFailures: [
      'data should be correctly decompressed even if input is detached partway',
    ],
  },
  'idlharness.https.any.js': {
    comment:
      'Workers expose globals differently than browsers - readable/writable attribute tests still fail',
    expectedFailures: [
      'CompressionStream interface: existence and properties of interface prototype object',
      'DecompressionStream interface: existence and properties of interface prototype object',
    ],
  },
  'third_party/pako/pako_inflate.min.js': {},
} satisfies TestRunnerConfig;
