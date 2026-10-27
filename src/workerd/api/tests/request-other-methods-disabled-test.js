// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// With the OTHER_HTTP_METHODS autogate disabled (this test has no @all-autogates variant),
// new Request() accepts only kj's listed methods. request-other-methods-test.js covers the gate
// being enabled.

import { strictEqual, throws } from 'node:assert';

// The config turns upper_case_all_http_methods off: with it on, every spelling of a listed method
// is uppercased before anything here applies (see fetch-test.js).
strictEqual(
  globalThis.Cloudflare.compatibilityFlags['upper_case_all_http_methods'],
  false
);

export const constructor = {
  test() {
    throws(() => new Request('http://example.org', { method: 'BLOCK' }), {
      name: 'TypeError',
      message: 'Invalid HTTP method string: BLOCK',
    });
    // Nor does it keep another spelling of a listed method, such as "patch", as written.
    throws(() => new Request('http://example.org', { method: 'patch' }), {
      name: 'TypeError',
      message: 'Invalid HTTP method string: patch',
    });
  },
};
