// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Bodies that a Durable Object sends: as a fetch() request body from within
// blockConcurrencyWhile(), and as a response after a storage write. Each body is longer than the
// most the runtime reads or writes while it holds the isolate lock once, so sending it takes the
// lock again, as the actor's input gate must allow. An in-memory body is sent straight from its
// source or, once its stream exists, through the stream, as a ReadableStream body is.
import assert from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

const kSize = 2 * 1024 * 1024 + 1;
const kKinds = ['string', 'Uint8Array'];

// How postInCriticalSection() sends an in-memory body: in fetch()'s init, in a Request, or in a
// Request whose body stream has been created.
const kSends = ['init', 'request', 'created'];

function makeBody(kind) {
  switch (kind) {
    case 'string':
      return 'x'.repeat(kSize);
    case 'Uint8Array':
      return new Uint8Array(kSize).fill(0x78);
    case 'ReadableStream': {
      let sent = 0;
      return new ReadableStream({
        pull(controller) {
          const size = Math.min(64 * 1024, kSize - sent);
          controller.enqueue(new Uint8Array(size).fill(0x78));
          sent += size;
          if (sent === kSize) controller.close();
        },
      });
    }
  }
  throw new Error(`unknown body kind ${kind}`);
}

export class Sender extends DurableObject {
  // Posts a body from within blockConcurrencyWhile(), whose callback awaits the response, which
  // the receiver sends only once it has read the whole body.
  async postInCriticalSection(kind, send) {
    return await this.ctx.blockConcurrencyWhile(async () => {
      const init = { method: 'POST', body: makeBody(kind) };
      let response;
      if (send === 'init') {
        response = await this.env.SELF.fetch('http://receiver/', init);
      } else {
        const request = new Request('http://receiver/', init);
        if (send === 'created') assert.ok(request.body);
        response = await this.env.SELF.fetch(request);
      }
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
      for (const send of kSends) {
        const stub = env.ns.getByName(`post-${kind}-${send}`);
        assert.strictEqual(
          await stub.postInCriticalSection(kind, send),
          kSize,
          `${kind} ${send}`
        );
      }
    }
  },
};

export const postStreamFromCriticalSection = {
  async test(ctrl, env) {
    const stub = env.ns.getByName('post-stream');
    assert.strictEqual(
      await stub.postInCriticalSection('ReadableStream', 'init'),
      kSize
    );
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
