// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'IdnaTestV2-removed.window.js': {},
  'IdnaTestV2.window.js': {},
  'historical.any.js': {},
  'idlharness.any.js': {
    comment: 'Implement web IDL support in harness',
    disabledTests: true,
  },
  'javascript-urls.window.js': {
    comment: 'Implement `globalThis.document`',
    expectedFailures: [
      'javascript: URL that fails to parse due to invalid host',
      'javascript: URL that fails to parse due to invalid host and has a U+0009 in scheme',
      'javascript: URL without an opaque path',
      'javascript: URL containing a JavaScript string split over path and query',
      'javascript: URL containing a JavaScript string split over path and query and has a U+000A in scheme',
    ],
  },
  'percent-encoding.window.js': {
    comment:
      'Implement test code modification feature to allow running this test without document',
    disabledTests: true,
  },
  'toascii.window.js': {
    replace: (code): string =>
      code.replace(/\["url", "a", "area"\]/, '[ "url" ]'),
  },
  'url-constructor.any.js': {},
  'url-origin.any.js': {},
  'url-searchparams.any.js': {},
  'url-setters-a-area.window.js': {
    comment:
      'Excluded because it uses the same test data as url-setters.any.js',
    // Node does the same: https://github.com/nodejs/node/blob/5ab7c4c5b01e7579fd436000232f0f0484289d44/test/wpt/status/url.json#L24
    omittedTests: true,
  },
  'url-setters-stripping.any.js': {},
  'url-setters.any.js': {},
  'url-statics-canparse.any.js': {},
  'url-statics-parse.any.js': {},
  'url-tojson.any.js': {},
  'urlencoded-parser.any.js': {
    comment:
      'Requests fail due to HTTP method "LADIDA", responses fail due to shift_jis encoding',
    expectedFailures: [
      /request.formData\(\) with input:/,
      /response.formData\(\) with input:/,
    ],
  },
  'urlsearchparams-append.any.js': {},
  'urlsearchparams-constructor.any.js': {
    comment: 'Fix this eventually',
    expectedFailures: ['URLSearchParams constructor, DOMException as argument'],
  },
  'urlsearchparams-delete.any.js': {},
  'urlsearchparams-foreach.any.js': {},
  'urlsearchparams-get.any.js': {},
  'urlsearchparams-getall.any.js': {},
  'urlsearchparams-has.any.js': {},
  'urlsearchparams-set.any.js': {},
  'urlsearchparams-size.any.js': {},
  'urlsearchparams-sort.any.js': {},
  'urlsearchparams-stringifier.any.js': {},
} satisfies TestRunnerConfig;
