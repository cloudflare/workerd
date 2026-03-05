// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'IdnaTestV2-removed.window.js': {},
  'IdnaTestV2.window.js': {},
  'historical.any.js': {},
  'idlharness.any.js': {
    comment:
      'IDL tests fail because Workers exposes globals differently than browsers (not as own properties of self)',
    expectedFailures: [
      'URL interface: existence and properties of interface object',
      'URL interface: attribute href',
      'URL interface: attribute origin',
      'URL interface: attribute protocol',
      'URL interface: attribute username',
      'URL interface: attribute password',
      'URL interface: attribute host',
      'URL interface: attribute hostname',
      'URL interface: attribute port',
      'URL interface: attribute pathname',
      'URL interface: attribute search',
      'URL interface: attribute searchParams',
      'URL interface: attribute hash',
      'URLSearchParams interface: existence and properties of interface object',
      'URLSearchParams interface object length',
      'URLSearchParams interface object name',
      'URLSearchParams interface: existence and properties of interface prototype object',
      'URLSearchParams interface: existence and properties of interface prototype object\'s "constructor" property',
      "URLSearchParams interface: existence and properties of interface prototype object's @@unscopables property",
      'URLSearchParams interface: attribute size',
      'URLSearchParams interface: operation append(USVString, USVString)',
      'URLSearchParams interface: operation delete(USVString, optional USVString)',
      'URLSearchParams interface: operation get(USVString)',
      'URLSearchParams interface: operation getAll(USVString)',
      'URLSearchParams interface: operation has(USVString, optional USVString)',
      'URLSearchParams interface: operation set(USVString, USVString)',
      'URLSearchParams interface: operation sort()',
      'URLSearchParams interface: iterable<USVString, USVString>',
      'URLSearchParams interface: stringifier',
      'URLSearchParams must be primary interface of new URLSearchParams("hi=there&thank=you")',
    ],
  },
  'javascript-urls.window.js': {
    comment: 'Implement `globalThis.document`',
    expectedFailures: [
      'javascript: URL that fails to parse due to invalid host',
      'javascript: URL that fails to parse due to invalid host and has a U+0009 in scheme',
      'javascript: URL without an opaque path',
      'javascript: URL containing a JavaScript string split over path and query',
      'javascript: URL containing a JavaScript string split over path and query and has a U+000A in scheme',
      'javascript: URL with extra slashes at the start',
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
