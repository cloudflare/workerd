// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// A socket opened with secureTransport 'starttls' offers the connect() handler at the other end a
// slot to fill in with that socket's startTls(). The slot must stay valid for as long as the
// handler can still be called. This tunnel outlives the request that opened it: that request
// returns the socket's readable as its response body, and a Durable Object that is still inside
// blockConcurrencyWhile() serves the tunnel only after the request has ended.

import { DurableObject, WorkerEntrypoint } from 'cloudflare:workers';
import { strictEqual } from 'node:assert';

export class Target extends DurableObject {
  // Locks the input gate until `release` ends, so that a connect() sent in the meantime waits to
  // be served. waitUntil() keeps this request current after it returns, to go on reading.
  hold(release) {
    this.ctx.waitUntil(
      this.ctx.blockConcurrencyWhile(() => new Response(release).text())
    );
  }

  async connect(socket) {
    // The tunnel came from a Socket that offered the slot. Seeing 'starttls' here means the
    // handler found the slot and set up its side of the upgrade.
    const writer = socket.writable.getWriter();
    await writer.write(new TextEncoder().encode(socket.secureTransport));
    await writer.close();
  }
}

export class Caller extends WorkerEntrypoint {
  // Ends its request without waiting for the tunnel to open, leaving the Socket behind with only
  // its readable still in use, by the response body.
  fetch() {
    const stub = this.env.TARGET.get(this.env.TARGET.idFromName('target'));
    const socket = stub.connect('smtp.example.com:25', {
      secureTransport: 'starttls',
    });
    return new Response(socket.readable);
  }
}

export const slotOutlivesTheRequestThatOpenedTheTunnel = {
  async test(ctrl, env) {
    // A promise settled from another request would reach the Durable Object only once its
    // input gate is open again, so the release has to come as I/O.
    const release = new TransformStream();
    const stub = env.TARGET.get(env.TARGET.idFromName('target'));
    await stub.hold(release.readable);

    // The Caller's request is over once its response arrives. The tunnel it opened is not served
    // yet: the Durable Object is still blocked.
    const response = await env.CALLER.fetch('http://caller.example/');
    await release.writable.close();

    strictEqual(await response.text(), 'starttls');
  },
};
