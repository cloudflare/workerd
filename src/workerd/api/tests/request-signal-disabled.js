// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { WorkerEntrypoint } from 'cloudflare:workers';
import assert from 'node:assert';
export class OtherServer extends WorkerEntrypoint {
  async fetch() {
    await scheduler.wait(300);
    return new Response('completed');
  }

  async rpcEcho(req) {
    return req;
  }
}
export class Server extends WorkerEntrypoint {
  async fetch(req) {
    const _resSameRequest = await this.env.OtherServer.rpcEcho(req);
    const _resNewRequest = await this.env.OtherServer.rpcEcho(new Request(req));
    const _resClonedRequest = await this.env.OtherServer.rpcEcho(req.clone());

    return new Response('ok');
  }
}

export const incomingRequestSignalCanBeCloned = {
  async test(ctrl, env, ctx) {
    const req = env.Server.fetch('http://example.com/cloneIncomingRequest');
    const res = await req;
    assert.strictEqual(await res.text(), 'ok');
  },
};
