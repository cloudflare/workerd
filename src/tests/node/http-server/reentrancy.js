// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Re-entering the request and response from their own events: destroying
// the response from 'finish', toggling the request's pause()/resume()
// inside 'data'.

import { strictEqual, deepStrictEqual } from 'node:assert';
import { withServer } from 'harness';

// destroy() from inside 'finish': the body, already handed off and closed,
// reaches the client whole; 'close' follows once, with the response
// destroyed and no 'error'.
export const destroyInsideFinish = {
  async test(ctrl, env) {
    const events = [];
    await withServer(
      (req, res) => {
        res.on('error', (err) => events.push(['error', err]));
        res.on('close', () => events.push(['close', res.destroyed]));
        res.on('finish', () => {
          events.push(['finish']);
          res.destroy();
        });
        res.end('done');
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        strictEqual(res.status, 200);
        strictEqual(await res.text(), 'done');
        await scheduler.wait(10);
        deepStrictEqual(events, [['finish'], ['close', true]]);
      }
    );
  },
};

// pause() then resume() inside every 'data': the body pump's one reader
// delivers every byte, none twice.
export const pauseResumeInsideData = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        let bytes = 0;
        req.on('data', (chunk) => {
          bytes += chunk.length;
          req.pause();
          req.resume();
        });
        req.on('end', () => res.end(String(bytes)));
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/', {
          method: 'POST',
          body: new Uint8Array(300_000),
        });
        strictEqual(await res.text(), '300000');
      }
    );
  },
};
