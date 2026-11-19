// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// In-memory bodies that a Durable Object sends: as a fetch() request body from within
// blockConcurrencyWhile(), and as a response after a storage write. Each body is longer than the
// most the runtime writes while it holds the isolate lock once, so sending it takes the lock
// again, as the actor's input gate allows.
import assert from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

const kSize = 2 * 1024 * 1024 + 1;
const kKinds = ['string', 'Uint8Array'];

function makeBody(kind) {
  switch (kind) {
    case 'string':
      return 'x'.repeat(kSize);
    case 'Uint8Array':
      return new Uint8Array(kSize).fill(0x78);
  }
  throw new Error(`unknown body kind ${kind}`);
}

export class Sender extends DurableObject {
  // Posts a body from within blockConcurrencyWhile(), whose callback awaits the response, which
  // the receiver sends only once it has read the whole body.
  async postInCriticalSection(kind) {
    return await this.ctx.blockConcurrencyWhile(async () => {
      const response = await this.env.SELF.fetch('http://receiver/', {
        method: 'POST',
        body: makeBody(kind),
      });
      return Number(await response.text());
    });
  }

  async fetch(request) {
    // The output gate holds the response until this write is stored.
    this.ctx.storage.put('responded', true);
    return new Response(
      makeBody(new URL(request.url).searchParams.get('kind'))
    );
  }

  async wasStored() {
    return await this.ctx.storage.get('responded');
  }
}

export default {
  // Reads the whole body, then responds with its length.
  async fetch(request) {
    return new Response(String((await request.arrayBuffer()).byteLength));
  },
};

export const postFromCriticalSection = {
  async test(ctrl, env) {
    for (const kind of kKinds) {
      const stub = env.ns.getByName(`post-${kind}`);
      assert.strictEqual(await stub.postInCriticalSection(kind), kSize, kind);
    }
  },
};

export const respondAfterStorageWrite = {
  async test(ctrl, env) {
    for (const kind of kKinds) {
      const stub = env.ns.getByName(`respond-${kind}`);
      const response = await stub.fetch(`http://sender/?kind=${kind}`);
      assert.strictEqual(
        (await response.arrayBuffer()).byteLength,
        kSize,
        kind
      );
      assert.strictEqual(await stub.wasStored(), true, kind);
    }
  },
};
