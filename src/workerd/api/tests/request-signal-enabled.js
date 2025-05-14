import { DurableObject, WorkerEntrypoint } from 'cloudflare:workers';
import assert from 'node:assert';

export class AbortTracker extends DurableObject {
  async getAborted(key) {
    return this.ctx.storage.get(key);
  }
  async setAborted(key, value) {
    await this.ctx.storage.put(key, value);
  }
}

let reqSignalReason = null;
let rpcSignalReason = null;
export class OtherServer extends WorkerEntrypoint {
  async fetch(req) {
    await scheduler.wait(300);
    return new Response('completed');
  }

  async echo(val) {
    return val;
  }

  async saveReqReason(req) {
    rpcSignalReason = req.signal.reason?.message;
  }
}

export class Server extends WorkerEntrypoint {
  async fetch(req) {
    const key = new URL(req.url).pathname.slice(1);
    let abortTracker = this.env.AbortTracker.get(
      this.env.AbortTracker.idFromName('AbortTracker')
    );
    await abortTracker.setAborted(key, false);

    req.signal.onabort = () => {
      this.ctx.waitUntil(abortTracker.setAborted(key, true));
    };

    return this[key](req);
  }

  async valid() {
    return new Response('hello world');
  }

  async error() {
    throw new Error('boom');
  }

  async hang() {
    for (;;) {
      await scheduler.wait(86400);
    }
  }

  async hangAfterSendingSomeData() {
    const { readable, writable } = new IdentityTransformStream();
    this.ctx.waitUntil(this.sendSomeData(writable));

    return new Response(readable);
  }

  async sendSomeData(writable) {
    const writer = writable.getWriter();
    const enc = new TextEncoder();
    await writer.write(enc.encode('hello world'));
    await this.hang();
  }

  async triggerSubrequest(req) {
    this.ctx.waitUntil(this.callOtherServer(req));
    await this.hang();
  }

  async callOtherServer(req) {
    const key = 'subrequest';

    let abortTracker = this.env.AbortTracker.get(
      this.env.AbortTracker.idFromName('AbortTracker')
    );

    const passedThroughReq = new Request(req);
    passedThroughReq.onabort = () => {
      this.ctx.waitUntil(abortTracker.setAborted(key, true));
    };

    const res = await this.env.OtherServer.fetch(passedThroughReq);
    const text = await res.text();

    if (text == 'completed') {
      await abortTracker.setAborted(key, false);
    }
  }

  async passIncomingRequestOverRpc(req) {
    // req.signal is in the "this signal" slot. It's an incoming request signal so it has the
    // IGNORE_FOR_SUBREQUESTS flag set.
    req.signal.onabort = () => {
      // If we're here we know that the reason is filled in on req.signal
      reqSignalReason = req.signal.reason.message;

      // Ensure that the incoming request signal is in the "signal" slot, which is the one we
      // actually serialize currently.
      const newReq = req.clone();

      // Since the signal is not included in serialization, OtherServer won't be able to read the
      // reason.
      this.ctx.waitUntil(this.env.OtherServer.saveReqReason(newReq));
    };

    // Just hang and wait for the client to give up
    await scheduler.wait(86400);
  }
}

export const noAbortOnSimpleResponse = {
  async test(ctrl, env, ctx) {
    let abortTracker = env.AbortTracker.get(
      env.AbortTracker.idFromName('AbortTracker')
    );

    const req = env.Server.fetch('http://example.com/valid');

    const res = await req;
    assert.strictEqual(await res.text(), 'hello world');
    assert.strictEqual(await abortTracker.getAborted('valid'), false);
  },
};

export const noAbortIfServerThrows = {
  async test(ctrl, env, ctx) {
    let abortTracker = env.AbortTracker.get(
      env.AbortTracker.idFromName('AbortTracker')
    );

    const req = env.Server.fetch('http://example.com/error');

    await assert.rejects(() => req, { name: 'Error', message: 'boom' });
    assert.strictEqual(await abortTracker.getAborted('error'), false);
  },
};

export const abortIfClientAbandonsRequest = {
  async test(ctrl, env, ctx) {
    let abortTracker = env.AbortTracker.get(
      env.AbortTracker.idFromName('AbortTracker')
    );

    // This endpoint never generates a response, so we can timeout after an arbitrary time.
    const req = env.Server.fetch('http://example.com/hang', {
      signal: AbortSignal.timeout(500),
    });

    await assert.rejects(() => req, {
      name: 'TimeoutError',
      message: 'The operation was aborted due to timeout',
    });
    assert.strictEqual(await abortTracker.getAborted('hang'), true);
  },
};

export const abortIfClientCancelsReadingResponse = {
  async test(ctrl, env, ctx) {
    let abortTracker = env.AbortTracker.get(
      env.AbortTracker.idFromName('AbortTracker')
    );

    // This endpoint begins generating a response but then hangs
    const req = env.Server.fetch('http://example.com/hangAfterSendingSomeData');
    const res = await req;
    const reader = res.body.getReader();

    const { value, done } = await reader.read();
    assert.strictEqual(new TextDecoder().decode(value), 'hello world');
    assert.ok(!done);

    // Give up reading
    await reader.cancel();

    // Waste a bit of time so the server cleans up
    await scheduler.wait(0);

    assert.strictEqual(
      await abortTracker.getAborted('hangAfterSendingSomeData'),
      true
    );
  },
};

export const abortedRequestDoesNotAbortSubrequest = {
  async test(ctrl, env, ctx) {
    let abortTracker = env.AbortTracker.get(
      env.AbortTracker.idFromName('AbortTracker')
    );

    // This endpoint calls another endpoint that eventually completes after wasting 300 ms
    // So, we abort the initial request quickly...
    const req = env.Server.fetch('http://example.com/triggerSubrequest', {
      signal: AbortSignal.timeout(100),
    });

    await assert.rejects(() => req, {
      name: 'TimeoutError',
      message: 'The operation was aborted due to timeout',
    });
    assert.strictEqual(
      await abortTracker.getAborted('triggerSubrequest'),
      true
    );

    // Then make sure that the subrequest wasn't also aborted
    await scheduler.wait(500);
    assert.strictEqual(await abortTracker.getAborted('subrequest'), false);
  },
};

export const requestPassedOverRpcDoesNotIncludeSignal = {
  async test(ctrl, env, ctx) {
    // This test covers the case when the `request_signal_passthrough` compat flag is not set.
    // The incoming request's signal is not serialized.
    // See request-signal-psssthrough.js for the corresponding test when the flag is set.

    await assert.rejects(
      () =>
        env.Server.fetch('http://example.com/passIncomingRequestOverRpc', {
          signal: AbortSignal.timeout(100),
        }),
      { message: 'The operation was aborted due to timeout' }
    );

    // Yield so the workers can write the abort reasons
    await scheduler.wait(0);

    assert.strictEqual(reqSignalReason, 'The client has disconnected');
    assert.strictEqual(rpcSignalReason, undefined);
  },
};
