// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'compression-bad-chunks.tentative.any.js': {
    comment:
      'V8 assertion - Cannot construct ArrayBuffer with a BackingStore of SharedArrayBuffer',
    skipAllTests: true,
  },
  'compression-constructor-error.tentative.any.js': {
    comment: 'TODO investigate this',
    expectedFailures: [
      'non-string input should cause the constructor to throw',
    ],
  },
  'compression-including-empty-chunk.tentative.any.js': {},
  'compression-large-flush-output.any.js': {},
  'compression-multiple-chunks.tentative.any.js': {},
  'compression-output-length.tentative.any.js': {
    comment: 'Check if this needs backend server',
    expectedFailures: [
      'the length of deflated (with -raw) data should be shorter than that of the original data',
      'the length of deflated data should be shorter than that of the original data',
      'the length of gzipped data should be shorter than that of the original data',
    ],
  },
  'compression-stream.tentative.any.js': {
    comment: 'Check if this needs backend server',
    expectedFailures:
      process.platform === 'win32'
        ? []
        : [
            'deflated small amount data should be reinflated back to its origin',
            'deflated large amount data should be reinflated back to its origin',
            'gzipped small amount data should be reinflated back to its origin',
            'gzipped large amount data should be reinflated back to its origin',
          ],
  },
  'compression-with-detach.tentative.window.js': {},
  'decompression-bad-chunks.tentative.any.js': {
    comment:
      'V8 assertion - Cannot construct ArrayBuffer with a BackingStore of SharedArrayBuffer',
    skipAllTests: true,
  },
  'decompression-buffersource.tentative.any.js': {
    comment: 'Enable once Float16Array is enabled',
    expectedFailures: [
      'chunk of type Float16Array should work for deflate',
      'chunk of type Float16Array should work for gzip',
      'chunk of type Float16Array should work for deflate-raw',
    ],
  },
  'decompression-constructor-error.tentative.any.js': {
    comment: 'TOOD investigate this',
    expectedFailures: [
      'non-string input should cause the constructor to throw',
    ],
  },
  'decompression-correct-input.tentative.any.js': {},
  'decompression-corrupt-input.tentative.any.js': {
    comment: 'TODO investigate this',
    expectedFailures: [
      "format 'deflate' field CMF should be error for 0",
      "format 'deflate' field FLG should be error for 157",
      "format 'deflate' field DATA should be error for 5",
      "format 'deflate' field ADLER should be error for 255",
      "format 'gzip' field ID should be error for 255",
      "format 'gzip' field CM should be error for 0",
      "format 'gzip' field FLG should be error for 2",
      "format 'gzip' field DATA should be error for 3",
      "format 'gzip' field CRC should be error for 0",
      "format 'gzip' field ISIZE should be error for 1",
      'the deflate input compressed with dictionary should give an error',
    ],
  },
  'decompression-empty-input.tentative.any.js': {},
  'decompression-split-chunk.tentative.any.js': {},
  'decompression-uint8array-output.tentative.any.js': {},
  'decompression-with-detach.tentative.window.js': {
    comment: 'Cannot redefine property: then',
    expectedFailures: [
      'data should be correctly decompressed even if input is detached partway',
    ],
  },
  'idlharness.https.any.js': {
    comment: 'Test file /resources/WebIDLParser.js not found',
    skipAllTests: true,
  },
  'third_party/pako/pako_inflate.min.js': {},
} satisfies TestRunnerConfig;
