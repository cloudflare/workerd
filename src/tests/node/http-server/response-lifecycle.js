// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tearing a response down early. Destroyed before the headers are sent, the
// fetch itself fails (with the destroy error, or a premature-close error);
// destroyed after, the body errors the same way. The body stream's cancel
// — a client abandoning the response — destroys the ServerResponse.
//
// Cancellation across the service binding only reaches the peer once the
// exchange completes, so the client-cancel tests dispatch an in-isolate
// Request, whose Response body is the very stream the handler feeds.

import { rejects } from 'node:assert';
import { withServer } from 'harness';

// destroy(err) before headers: the fetch rejects with that error.
export const destroyWithErrorBeforeHeadersRejectsFetch = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        res.destroy(new Error('gone before headers'));
      },
      async () => {
        await rejects(env.SERVICE.fetch('http://x/'), {
          message: 'gone before headers',
        });
      }
    );
  },
};
