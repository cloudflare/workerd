// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual } from 'node:assert';
import { env } from 'cloudflare:workers';

// WPT covers the immutable guard under pedantic_wpt. This covers what it can't: Response.error()
// outside of an IoContext, and mutable headers without pedantic_wpt.
const globalScopeResponse = Response.error();

export const errorHeaders = {
  test() {
    strictEqual(globalScopeResponse.type, 'error');
    if (!env.pedantic) {
      const response = Response.error();
      response.headers.append('foo', 'bar');
      strictEqual(response.headers.get('foo'), 'bar');
    }
  },
};
