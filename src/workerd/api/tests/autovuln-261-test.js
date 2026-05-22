// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-261.
// Piping a JS ReadableStream to an internal writable with { signal, preventAbort: true }
// then aborting the signal during pull() must not access the source PipeController&
// after checkSignal releases it.
export const pipeAbortSignalPreventAbortNoUAF = {
  async test() {
    const ac = new AbortController();
    let cancelCalled = false;

    const rs = new ReadableStream(
      {
        pull(controller) {
          ac.abort(new Error('boom'));
          controller.enqueue(new Uint8Array([1, 2, 3]));
        },
        cancel(reason) {
          cancelCalled = true;
        },
      },
      { highWaterMark: 0 }
    );

    const cs = new CompressionStream('gzip');

    let rejected = false;
    await rs
      .pipeTo(cs.writable, { signal: ac.signal, preventAbort: true })
      .then(
        () => {
          throw new Error('should have rejected');
        },
        (e) => {
          rejected = true;
          strictEqual(e.message, 'boom');
        }
      );

    strictEqual(rejected, true, 'pipeTo should have rejected');
    strictEqual(
      cancelCalled,
      true,
      'cancel should have been called on the source'
    );
  },
};
