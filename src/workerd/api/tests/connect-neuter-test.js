// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-334:
// connect() handler must neuter the NeuterableIoStream when
// the handler promise resolves, preventing use-after-free.

import { rejects } from 'assert';

// Set by the connect handler's waitUntil() task once its late write has been attempted: true if
// the write was rejected as expected, otherwise the assertion error.
let lateWriteResult;

export default {
  async connect(socket, env, ctx) {
    const writer = socket.writable.getWriter();

    ctx.waitUntil(
      (async () => {
        // Allow connect to return before attempting the write. This should result in the stream
        // being neutered.
        await scheduler.wait(0);

        try {
          await rejects(
            async () => await writer.write(new Uint8Array([0x41, 0x42])),
            {
              name: 'TypeError',
              message:
                "Can't read from request stream because client disconnected.",
            }
          );
          lateWriteResult = true;
        } catch (e) {
          lateWriteResult = e;
        }
      })()
    );

    return;
  },
};

export const connectNeuterRegression = {
  async test(ctrl, env) {
    const socket = env.SELF.connect('example.com:1234');

    // The destination will close the socket when its `connect` returns.
    await socket.closed;

    // The late write runs in the connect handler's waitUntil() task, which under load can finish
    // well after the socket closes, so wait for its result rather than for a fixed time. The bound
    // is far beyond what the task needs; it only turns a write that never completes into a clear
    // failure before the test target's own timeout.
    for (let i = 0; lateWriteResult === undefined && i < 10_000; ++i) {
      await scheduler.wait(1);
    }
    if (lateWriteResult === undefined) {
      throw new Error('timed out waiting for the late write to be attempted');
    }
    if (lateWriteResult !== true) {
      throw lateWriteResult;
    }
  },
};
