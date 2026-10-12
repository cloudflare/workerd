// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-334:
// connect() handler must neuter the NeuterableIoStream when
// the handler promise resolves, preventing use-after-free.

import { strictEqual, rejects } from 'assert';

let writeRejected = false;
let lateWrite;

export default {
  async connect(socket, env, ctx) {
    const writer = socket.writable.getWriter();

    lateWrite = (async () => {
      // Yield until connect returns, resuming through this handler's I/O context.
      await scheduler.wait(0);

      await rejects(
        async () => await writer.write(new Uint8Array([0x41, 0x42])),
        (error) => {
          strictEqual(error.name, 'TypeError', error.stack);
          strictEqual(
            error.message,
            "Can't read from request stream because client disconnected.",
            error.stack
          );
          return true;
        }
      );

      writeRejected = true;
    })();
    ctx.waitUntil(lateWrite);

    return;
  },
};

export const connectNeuterRegression = {
  async test(ctrl, env) {
    const socket = env.SELF.connect('example.com:1234');

    // The destination will close the socket when its `connect` returns.
    await socket.closed;

    await lateWrite;

    strictEqual(writeRejected, true, 'write must throw on a neutered stream');
  },
};
