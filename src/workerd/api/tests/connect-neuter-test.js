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
const { promise: socketClosed, resolve: signalSocketClosed } =
  Promise.withResolvers();

export default {
  async connect(socket, env, ctx) {
    const writer = socket.writable.getWriter();

    lateWrite = (async () => {
      // The test signals after the connect handler returns and the peer observes closure.
      await socketClosed;

      await rejects(
        async () => await writer.write(new Uint8Array([0x41, 0x42])),
        {
          name: 'TypeError',
          message:
            "Can't read from request stream because client disconnected.",
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

    signalSocketClosed();
    await lateWrite;

    strictEqual(writeRejected, true, 'write must throw on a neutered stream');
  },
};
