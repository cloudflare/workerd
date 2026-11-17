// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

// The compression WPT suite against the TypeScript streams implementation's
// CompressionStream/DecompressionStream pair (webstreams/compression.ts over
// the shared C++ CodecStage). Expectations match the legacy configuration
// (compression-test.ts) except where the TypeScript pair follows the spec
// and the legacy one does not (bad chunks; IDL prototype attributes).
export default {
  // The legacy configuration disables this file (the C++ pair accepts
  // SharedArrayBuffer chunks and keeps the stream usable after an invalid
  // one); the TypeScript pair rejects both per spec.
  'compression-bad-chunks.any.js': {},
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
  'decompression-extra-input.any.js': {},
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
  // The interface-prototype subtests that fail against the legacy classes (see
  // compression-test.ts) pass against the TypeScript pair: its prototype property
  // attributes follow the IDL rules.
  'idlharness.https.any.js': {},
  'third_party/pako/pako_inflate.min.js': {},
} satisfies TestRunnerConfig;
