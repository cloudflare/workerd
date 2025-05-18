import { WorkerEntrypoint } from 'cloudflare:workers';
import assert from 'node:assert';

let reqSignalReason = null;
let rpcSignalReason = null;

export class OtherServer extends WorkerEntrypoint {
  async echo(val) {
    return val;
  }

  async saveReqReason(req) {
    rpcSignalReason = req.signal.reason?.message;
  }
}

export class Server extends WorkerEntrypoint {
  async fetch(req) {
    // req.signal is in the "this signal" slot. Because request_signal_passthrough is set, there is
    // no special handling.
    req.signal.onabort = () => {
      // If we're here we know that the reason is filled in on req.signal
      reqSignalReason = req.signal.reason.message;

      // Ensure that the incoming request signal is in the "signal" slot, which is the one we
      // actually serialize currently.
      const newReq = req.clone();

      // And if the signal really is included in serialization, OtherServer will be able to read
      // the reason.
      this.ctx.waitUntil(this.env.OtherServer.saveReqReason(newReq));
    };

    // Just hang and wait for the client to give up
    await scheduler.wait(86400);
  }
}

export const requestPassedOverRpcIncludesSignal = {
  async test(ctrl, env, ctx) {
    await assert.rejects(
      () =>
        env.Server.fetch('http://example.com', {
          signal: AbortSignal.timeout(100),
        }),
      { message: 'The operation was aborted due to timeout' }
    );

    // Yield so the workers can write the abort reasons
    await scheduler.wait(0);

    assert.strictEqual(reqSignalReason, 'The client has disconnected');
    assert.strictEqual(rpcSignalReason, 'The client has disconnected');
  },
};
