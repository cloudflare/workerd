// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A Durable Object's IoContext outlives its requests, so a body it transforms in one request can
// fail after that request has responded and be read in a later one. Reading it then rejects with
// what the source failed with.

import assert from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

// Responds with a body that fails once the request that fetched it is over.
export const source = {
  async fetch() {
    return new Response(
      new ReadableStream({
        async pull(controller) {
          await scheduler.wait(100);
          controller.error(new Error('the source failed'));
        },
      })
    );
  },
};

export class Holder extends DurableObject {
  #body;

  async fetch(request) {
    if (new URL(request.url).pathname === '/start') {
      const response = await this.env.SOURCE.fetch('http://source/');
      this.#body = new HTMLRewriter()
        .on('p', { element() {} })
        .transform(response).body;
      return new Response('started');
    }
    try {
      return new Response(`read: ${await new Response(this.#body).text()}`);
    } catch (error) {
      return new Response(`rejected: ${error.message}`);
    }
  }
}

export const abortBetweenRequests = {
  async test(controller, env) {
    const stub = env.HOLDER.get(env.HOLDER.idFromName('holder'));
    assert.strictEqual(
      await (await stub.fetch('http://holder/start')).text(),
      'started'
    );
    // Long enough for the source to fail before the next request.
    await scheduler.wait(500);
    const read = stub.fetch('http://holder/read').then((r) => r.text());
    const result = await Promise.race([
      read,
      scheduler.wait(5000).then(() => 'hung'),
    ]);
    assert.strictEqual(result, 'rejected: the source failed');
  },
};
