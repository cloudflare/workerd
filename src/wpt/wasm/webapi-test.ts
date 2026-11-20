// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

const differentPort =
  "The harness's get_host_info() has no HTTP_ORIGIN_WITH_DIFFERENT_PORT, so the cross-origin fetch doesn't reach the WPT server";

export default {
  'abort.any.js': {},
  'body.any.js': {},
  'contenttype.any.js': {},
  'empty-body.any.js': {},
  'historical.any.js': {
    comment: 'Uses indexedDB, which workerd does not provide',
    omittedTests: true,
  },
  'idlharness.any.js': {},
  'instantiateStreaming-bad-imports.any.js': {},
  'instantiateStreaming.any.js': {},
  'invalid-args.any.js': {},
  'invalid-code.any.js': {},
  'modified-contenttype.any.js': {},
  'origin.sub.any.js': {},
  'rejected-arg.any.js': {},
  'status.any.js': {},
  'wasm_stream_compile_test.any.js': {
    comment: differentPort,
    expectedFailures: ['compileStreaming check CORS'],
  },
  'wasm_stream_instantiate_test.any.js': {
    comment: differentPort,
    expectedFailures: ['instantiateStreaming check CORS'],
  },
} satisfies TestRunnerConfig;
