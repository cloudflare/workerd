// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual } from 'node:assert';

// A Response with no statusText has an empty status message, as the Fetch spec requires, on
// every compatibility date. These Workers run without pedantic_wpt, which WPT always enables.
export const emptyByDefault = {
  test() {
    strictEqual(new Response(null, { status: 404 }).statusText, '');
    strictEqual(Response.json({}).statusText, '');
    strictEqual(Response.redirect('http://example.com/').statusText, '');
    strictEqual(
      new Response(null, { statusText: 'Custom' }).statusText,
      'Custom'
    );
  },
};

// The HTTP status line still carries the status code's conventional reason phrase when no
// statusText is set. The target service returns Responses built from the query parameters, and
// fetch() reports the reason phrase it received. WPT can't observe what a Worker sends.
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
