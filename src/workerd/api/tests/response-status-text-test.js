// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual } from 'node:assert';

// Under pedantic_wpt, a Response with no statusText has an empty status message, but the HTTP
// status line still carries the status code's conventional reason phrase. The target service
// returns Responses built from the query parameters, and fetch() reports the reason phrase it
// received. WPT covers the JS-visible statusText; it can't observe what a Worker sends.
export const reasonPhraseOnTheWire = {
  async test(ctrl, env) {
    const url = (params) =>
      `http://target/?${new URLSearchParams(params).toString()}`;

    strictEqual(
      (await env.target.fetch(url({ status: 404 }))).statusText,
      'Not Found'
    );
    strictEqual(
      (await env.target.fetch(url({ status: 200, statusText: 'Custom' })))
        .statusText,
      'Custom'
    );
  },
};
