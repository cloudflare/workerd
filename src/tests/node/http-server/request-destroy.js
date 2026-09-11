// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Destroying the IncomingMessage: 'aborted' when the body was not complete,
// 'error' only for listeners, 'close' always; and the body stream underneath
// is cancelled so the producer learns of it.

import { strictEqual } from 'node:assert';
import { withServer } from 'harness';

// destroy(err) with an 'error' listener: the error is delivered and the
// handler can still respond.
export const destroyWithErrorEmitsError = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        req.on('error', (err) => {
          res.statusCode = 400;
          res.end(`Request destroyed: ${err.message}`);
        });
        req.destroy(new Error('Destroy test'));
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        strictEqual(res.status, 400);
        strictEqual(await res.text(), 'Request destroyed: Destroy test');
      }
    );
  },
};

// destroy() without an error: 'close' fires, no 'error'.
export const destroyWithoutErrorClosesQuietly = {
  async test(ctrl, env) {
    let errors = 0;
    await withServer(
      (req, res) => {
        req.once('error', () => errors++);
        req.on('close', () => {
          res.statusCode = 200;
          res.end('Request destroyed without error');
        });
        req.destroy();
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        strictEqual(res.status, 200);
        strictEqual(await res.text(), 'Request destroyed without error');
        strictEqual(errors, 0);
      }
    );
  },
};

// destroy(err) with no 'error' listener is swallowed: the request still
// closes and the response can be sent.
export const destroyWithErrorAndNoListenerIsSwallowed = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        req.on('close', () => res.end('closed'));
        req.destroy(new Error('nobody listens'));
      },
      async () => {
        strictEqual(
          await (await env.SERVICE.fetch('http://x/')).text(),
          'closed'
        );
      }
    );
  },
};
