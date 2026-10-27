// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// HTTP methods outside kj's list with upper_case_all_http_methods on, and the OTHER_HTTP_METHODS
// autogate enabled by this test's config. request-other-methods-test.js covers the flag being off.

import { strictEqual, throws } from 'node:assert';

export const upperCaseAll = {
  test() {
    // A method outside kj's list is uppercased like a listed one, so that it doesn't change when
    // kj starts listing it.
    for (const method of ['block', 'Block', 'BLOCK']) {
      strictEqual(
        new Request('http://example.org', { method }).method,
        'BLOCK'
      );
    }

    // Listed methods are uppercased in any letter case, as before.
    for (const [method, expected] of [
      ['poSt', 'POST'],
      ['patCh', 'PATCH'],
    ]) {
      strictEqual(
        new Request('http://example.org', { method }).method,
        expected
      );
    }

    // CONNECT and TRACK, which the Fetch spec forbids, are still rejected in any letter case.
    for (const method of ['connect', 'Track', 'track']) {
      throws(() => new Request('http://example.org', { method }), {
        name: 'TypeError',
        message: `Invalid HTTP method string: ${method}`,
      });
    }
  },
};
