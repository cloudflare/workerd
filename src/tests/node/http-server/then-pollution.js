// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Promise.prototype.then patched by user code: the server's body pump and
// the response's message buffer, which use no primordials, run through the
// patched then; a transparent patch changes nothing about the data.

import { strictEqual } from 'node:assert';
import { withServer } from 'harness';

// A transparent then sees the exchange's hops (the count is not pinned);
// the request body and the response body are intact.
export const patchedThenPassthroughKeepsData = {
  async test(ctrl, env) {
    const original = Promise.prototype.then;
    let calls = 0;
    Promise.prototype.then = function (onFulfilled, onRejected) {
      calls++;
      return original.call(this, onFulfilled, onRejected);
    };
    try {
      await withServer(
        (req, res) => {
          let body = '';
          req.setEncoding('utf8');
          req.on('data', (chunk) => (body += chunk));
          req.on('end', () => {
            res.writeHead(200);
            res.write('echo:');
            res.end(body);
          });
        },
        async () => {
          const res = await env.SERVICE.fetch('http://x/', {
            method: 'POST',
            body: 'hello',
          });
          strictEqual(await res.text(), 'echo:hello');
        }
      );
    } finally {
      Promise.prototype.then = original;
    }
    strictEqual(calls > 0, true);
  },
};
