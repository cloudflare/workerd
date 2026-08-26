// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Identity streams as Request/Response bodies — the typical usage scenario
// they exist for. Behavior is shared between the implementations except
// where noted: consuming the body drives the rendezvous exactly like a
// direct reader (under the TypeScript implementation this exercises the
// C++ bridge conduit for real), and FixedLengthStream enforcement surfaces
// through body consumption with the ledger #11 error-type divergence.

import { ok, strictEqual, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { writePatternedBody, assertPatternedBytes } from 'payload-helpers';

async function feed(writable, text) {
  const writer = writable.getWriter();
  await writer.write(new TextEncoder().encode(text));
  await writer.close();
}

// Large enough to force many rendezvous cycles and internal buffer
// boundary crossings; deliberately not chunk-aligned (odd tail) with a
// chunk length that is prime and larger than the C++ internal 4096-byte
// read buffer.
const LARGE_TOTAL = 2 * 1024 * 1024 + 13;
const LARGE_CHUNK = 65_521;

export const responseTextReadsIdentityBody = {
  async test() {
    const its = new IdentityTransformStream();
    const resp = new Response(its.readable);
    const feedPromise = feed(its.writable, 'response body');
    strictEqual(await resp.text(), 'response body');
    // The writes settled because text() consumed them: the rendezvous
    // holds through body consumption.
    await feedPromise;
  },
};

export const responseBodyIsTheSameStream = {
  async test() {
    // Constructing the Response neither wraps, consumes, nor locks the
    // stream: body is the very same ReadableStream object.
    const its = new IdentityTransformStream();
    const resp = new Response(its.readable);
    strictEqual(resp.body, its.readable);
    strictEqual(resp.bodyUsed, false);
    strictEqual(its.readable.locked, false);
    // And it is readable through the body property.
    const feedPromise = feed(its.writable, 'xy');
    const reader = resp.body.getReader();
    const { value, done } = await reader.read();
    strictEqual(done, false);
    strictEqual(new TextDecoder().decode(value), 'xy');
    strictEqual((await reader.read()).done, true);
    await feedPromise;
  },
};

export const requestWithIdentityBody = {
  async test() {
    const its = new IdentityTransformStream();
    const req = new Request('https://example.org/', {
      method: 'POST',
      body: its.readable,
    });
    const feedPromise = feed(its.writable, 'request body');
    strictEqual(await req.text(), 'request body');
    await feedPromise;
  },
};

export const fixedLengthResponseText = {
  async test() {
    // The FixedLengthStream headline use case: a body with a declared
    // total, delivered exactly.
    const fls = new FixedLengthStream(11);
    const resp = new Response(fls.readable);
    const feedPromise = feed(fls.writable, 'fixed bytes');
    strictEqual(await resp.text(), 'fixed bytes');
    await feedPromise;
  },
};

export const largeResponseBody = {
  async test() {
    // Multi-megabyte body: many rendezvous cycles, chunk boundaries
    // everywhere, verified byte-for-byte. arrayBuffer() rather than text()
    // so the comparison is on raw bytes.
    const its = new IdentityTransformStream();
    const bodyPromise = new Response(its.readable).arrayBuffer();
    await writePatternedBody(its.writable, LARGE_TOTAL, LARGE_CHUNK);
    assertPatternedBytes(new Uint8Array(await bodyPromise), LARGE_TOTAL);
  },
};

export const largeRequestBody = {
  async test() {
    const its = new IdentityTransformStream();
    const req = new Request('https://example.org/', {
      method: 'POST',
      body: its.readable,
    });
    const bodyPromise = req.arrayBuffer();
    await writePatternedBody(its.writable, LARGE_TOTAL, LARGE_CHUNK);
    assertPatternedBytes(new Uint8Array(await bodyPromise), LARGE_TOTAL);
  },
};

export const largeFixedLengthResponseBody = {
  async test() {
    // The FLS byte budget is decremented chunk by chunk across the whole
    // body; an off-by-anything in that arithmetic surfaces as an
    // enforcement error or a wrong total here.
    const fls = new FixedLengthStream(LARGE_TOTAL);
    const bodyPromise = new Response(fls.readable).arrayBuffer();
    await writePatternedBody(fls.writable, LARGE_TOTAL, LARGE_CHUNK);
    assertPatternedBytes(new Uint8Array(await bodyPromise), LARGE_TOTAL);
  },
};

export const fixedLengthUnderwriteRejectsBodyRead = {
  async test() {
    // Underwrite enforcement surfaces through body consumption, with the
    // ledger #11 divergence: C++ enforces on the read side with TypeError,
    // TypeScript eagerly with RangeError; the message is shared.
    const fls = new FixedLengthStream(10);
    const resp = new Response(fls.readable);
    const writer = fls.writable.getWriter();
    const feedPromise = (async () => {
      await writer.write(new TextEncoder().encode('abc'));
      await writer.close().catch(() => {});
    })();
    await rejects(resp.text(), (err) => {
      ok(err instanceof (usingTsImpl ? RangeError : TypeError));
      return /did not see all expected bytes/.test(err.message);
    });
    await feedPromise;
  },
};
