// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'compression-bad-chunks.tentative.any.js': {
    comment:
      'V8 assertion - Cannot construct ArrayBuffer with a BackingStore of SharedArrayBuffer',
    disabledTests: true,
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
    comment:
      'These tests require the sidecar which is not enabled for compression-test',
    disabledTests: [
      'the length of deflated (with -raw) data should be shorter than that of the original data',
      'the length of deflated data should be shorter than that of the original data',
      'the length of gzipped data should be shorter than that of the original data',
    ],
  },
  'compression-stream.tentative.any.js': {
    comment:
      'These tests require the sidecar which is not enabled for compression-test',
    disabledTests: [
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
    disabledTests: true,
  },
  'decompression-buffersource.tentative.any.js': {},
  'decompression-constructor-error.tentative.any.js': {
    comment: 'TODO investigate this',
    expectedFailures: [
      'non-string input should cause the constructor to throw',
    ],
  },
  'decompression-correct-input.tentative.any.js': {},
  'decompression-corrupt-input.tentative.any.js': {},
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
    disabledTests: true,
  },
  'third_party/pako/pako_inflate.min.js': {},
} satisfies TestRunnerConfig;
