// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'append.any.js': {},
  'constructor.any.js': {
    comment:
      'The FormData constructor takes no arguments, so new FormData(null) and ' +
      'new FormData("string") are accepted instead of throwing a TypeError',
    expectedFailures: ['Constructors should throw a type error'],
  },
  'delete.any.js': {},
  'foreach.any.js': {},
  'get.any.js': {},
  'has.any.js': {},
  'iteration.any.js': {},
  'set-blob.any.js': {
    comment:
      'A Blob added without a filename is named after the entry ("blob-1") instead of "blob", ' +
      'and a File added with a new filename loses its lastModified',
    expectedFailures: [
      'blob without type',
      'blob with type',
      'file with lastModified and custom name',
    ],
  },
  'set.any.js': {},
} satisfies TestRunnerConfig;
