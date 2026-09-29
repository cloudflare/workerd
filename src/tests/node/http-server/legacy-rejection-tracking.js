// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Without unhandled_rejection_after_microtask_checkpoint, the unhandled-
// rejection tracker reports a promise the moment it rejects, before a
// handler attached later in the same checkpoint can run. The C++
// implementation settles a read pending at cancel() through such a promise
// — the read rejects with the cancel reason, adopted by the read() promise
// a tick later — so req.destroy() with the body pump's read pending
// underneath (the handler paused the message and primed a read; the body
// arrives across the service binding, a stream the runtime provides)
// surfaces a spurious 'unhandledrejection' carrying "Stream was cancelled."
// even though the IncomingMessage catches the read's rejection. The message
// itself aborts as usual and the response is still sent.

import { strictEqual, deepStrictEqual } from 'node:assert';
import { withServer, collectUncaught } from 'harness';

export const legacyPendingReadCancelMisfiresAsUnhandledRejection = {
  async test(ctrl, env) {
    // An IdentityTransformStream needs no constructor flag; its readable is
    // the upload, held open until the exchange is done.
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const events = [];
    const leaked = await collectUncaught(() =>
      withServer(
        (req, res) => {
          req.on('aborted', () => events.push('aborted'));
          req.on('error', (err) => events.push(`error:${err.message}`));
          req.on('close', () => events.push(`close:${req.complete}`));
          req.pause();
          req.read(0);
          setTimeout(() => {
            req.destroy();
            res.writeHead(204);
            res.end();
          }, 5);
        },
        async () => {
          void writer.write(new TextEncoder().encode('one'));
          const res = await env.SERVICE.fetch('http://x/', {
            method: 'POST',
            body: readable,
          });
          strictEqual(res.status, 204);
          await writer.close().catch(() => {});
          await scheduler.wait(10);
          deepStrictEqual(events, ['aborted', 'close:false']);
        }
      )
    );
    strictEqual(leaked.length, 1);
    strictEqual(leaked[0].message, 'Stream was cancelled.');
  },
};
