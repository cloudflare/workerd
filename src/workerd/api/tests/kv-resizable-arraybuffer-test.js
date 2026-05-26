// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for resizable ArrayBuffer passed to KV.put().
// KV.put converts its value to kj::Array<byte> via jsg::asBytes(), which must
// deep-copy resizable buffers to prevent SIGSEGV from page decommit after
// resize(0). Ref: AUTOVULN-CLOUDFLARE-WORKERD-73
//
// This is also the mock KV backend. It stores PUT bodies in memory and returns
// them on GET, so the test can verify the data that was actually transmitted.

import assert from 'node:assert';
import { WorkerEntrypoint } from 'cloudflare:workers';

// In-memory store shared across requests within the same isolate.
const store = new Map();

export default class KVMock extends WorkerEntrypoint {
  async fetch(request) {
    const { pathname } = new URL(request.url);
    const key = decodeURIComponent(pathname.slice(1)); // strip leading /
    if (request.method === 'PUT') {
      store.set(key, await request.arrayBuffer());
      return new Response(null, { status: 200 });
    } else if (request.method === 'GET') {
      const data = store.get(key);
      if (data === undefined) {
        return new Response(null, { status: 404 });
      }
      return new Response(data, { status: 200 });
    }
    return new Response(null, { status: 405 });
  }
}

// Sophie's example: put a resizable ArrayBuffer into KV, mutate it after put
// but before await, then verify what KV received.
export const kvPutResizableArrayBuffer = {
  async test(ctrl, env, ctx) {
    const body = new ArrayBuffer(7, { maxByteLength: 16 });
    new TextEncoder().encodeInto('initial', new Uint8Array(body));
    const promise = env.KV.put('blah', body);
    new TextEncoder().encodeInto('changed', new Uint8Array(body));
    await promise;

    // Verify KV received "initial" (the data at put-time), not "changed".
    const stored = await env.KV.get('blah');
    assert.strictEqual(stored, 'initial');
  },
};

// Same test but resize the buffer to 0 after put — the original SIGSEGV vector.
export const kvPutResizableArrayBufferThenShrink = {
  async test(ctrl, env, ctx) {
    const body = new ArrayBuffer(64, { maxByteLength: 128 });
    const view = new Uint8Array(body);
    for (let i = 0; i < 64; i++) view[i] = i;

    const promise = env.KV.put('shrink-test', body);
    body.resize(0); // decommits pages — would SIGSEGV without deep copy
    await promise;

    // Verify KV received all 64 bytes.
    const stored = await env.KV.get('shrink-test', { type: 'arrayBuffer' });
    assert.strictEqual(stored.byteLength, 64);
    const result = new Uint8Array(stored);
    for (let i = 0; i < 64; i++) {
      assert.strictEqual(result[i], i, `byte ${i}`);
    }
  },
};

// Non-resizable buffer: does KV see the mutation that happens after put() but
// before await?  This settles whether KJ's .then() on an immediately-ready
// promise fires synchronously or defers to the event loop.
export const kvPutNonResizableMutateAfterPut = {
  async test(ctrl, env, ctx) {
    const body = new ArrayBuffer(7);
    new TextEncoder().encodeInto('initial', new Uint8Array(body));
    const promise = env.KV.put('non-rab-mutate', body);
    new TextEncoder().encodeInto('changed', new Uint8Array(body));
    await promise;

    const stored = await env.KV.get('non-rab-mutate');
    // If KV sees 'changed', the .then() callback that writes the HTTP body
    // ran AFTER our encodeInto — i.e. .then() deferred even on READY_NOW.
    // If KV sees 'initial', .then() fired inline during put().
    if (stored === 'changed') {
      console.log('KV.put .then() is DEFERRED: saw mutation after put()');
    } else if (stored === 'initial') {
      console.log('KV.put .then() is SYNCHRONOUS: did not see mutation after put()');
    }
    // Either way, this test should not crash.  Log the result so we can see
    // which behaviour we get.  Accept both for now.
    assert.ok(stored === 'initial' || stored === 'changed',
      `expected 'initial' or 'changed', got '${stored}'`);
  },
};
