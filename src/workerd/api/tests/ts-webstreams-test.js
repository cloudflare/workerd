// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0  workerd/require-copyright-header
const {
  ReadableStream,
  ReadableStreamDefaultReader,
  ReadableStreamBYOBReader,
  ReadableStreamDefaultController,
  ReadableByteStreamController,
  ReadableStreamBYOBRequest,
  ByteLengthQueuingStrategy,
  CountQueuingStrategy,
  WritableStream,
  WritableStreamDefaultWriter,
  WritableStreamDefaultController,
  TransformStream,
  TransformStreamDefaultController,
  IdentityTransformStream,
  FixedLengthStream,
  TextEncoderStream,
  TextDecoderStream,
} = globalThis;

import { doesNotMatch, ok, strictEqual, throws } from 'node:assert';

export const existenceTest = {
  test() {
    // None of the classes should report as "native code"
    doesNotMatch(ReadableStream.toString(), /\[native code\]/);
    doesNotMatch(ReadableStreamDefaultReader.toString(), /\[native code\]/);
    doesNotMatch(ReadableStreamBYOBReader.toString(), /\[native code\]/);
    doesNotMatch(ReadableStreamDefaultController.toString(), /\[native code\]/);
    doesNotMatch(ReadableByteStreamController.toString(), /\[native code\]/);
    doesNotMatch(ReadableStreamBYOBRequest.toString(), /\[native code\]/);
    doesNotMatch(ByteLengthQueuingStrategy.toString(), /\[native code\]/);
    doesNotMatch(CountQueuingStrategy.toString(), /\[native code\]/);
    doesNotMatch(WritableStream.toString(), /\[native code\]/);
    doesNotMatch(WritableStreamDefaultWriter.toString(), /\[native code\]/);
    doesNotMatch(WritableStreamDefaultController.toString(), /\[native code\]/);
    doesNotMatch(TransformStream.toString(), /\[native code\]/);
    doesNotMatch(
      TransformStreamDefaultController.toString(),
      /\[native code\]/
    );
    doesNotMatch(IdentityTransformStream.toString(), /\[native code\]/);
    doesNotMatch(FixedLengthStream.toString(), /\[native code\]/);
    doesNotMatch(TextEncoderStream.toString(), /\[native code\]/);
    doesNotMatch(TextDecoderStream.toString(), /\[native code\]/);
  },
};

// ======================================================================================
// C++-created streams (JsReadableStream::create()) are backed by the TypeScript
// implementation under the typescript_implemented_streams flag: the C++ side wraps its
// kj-native source in a ReadableStreamNativeSource (born carrying the kNativeSource
// marker) and constructs the TypeScript ReadableStream over it. Blob.prototype.stream()
// is the simplest C++ API that mints a stream this way, so these tests drive the real
// native-source pull/cancel contract end to end.

export const nativeBackedStreamIsTsStream = {
  test() {
    const stream = new Blob(['hello world']).stream();
    // The C++-created stream is an instance of the TypeScript-implemented class (the
    // legacy C++ implementation's instances would fail this brand check).
    ok(stream instanceof ReadableStream);
  },
};

export const nativeBackedDefaultRead = {
  async test() {
    const stream = new Blob(['hello world']).stream();
    const reader = stream.getReader();
    const decoder = new TextDecoder();
    let text = '';
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      text += decoder.decode(value, { stream: true });
    }
    text += decoder.decode();
    strictEqual(text, 'hello world');
  },
};

export const nativeBackedByobRead = {
  async test() {
    const stream = new Blob(['hello world']).stream();
    // Native-backed streams are byte-capable by definition: BYOB readers always work.
    const reader = stream.getReader({ mode: 'byob' });
    const { done, value } = await reader.read(new Uint8Array(64));
    strictEqual(done, false);
    strictEqual(new TextDecoder().decode(value), 'hello world');
    const eof = await reader.read(new Uint8Array(16));
    strictEqual(eof.done, true);
  },
};

export const nativeBackedMinReadUnderDelivery = {
  async test() {
    const stream = new Blob(['hello world']).stream();
    const reader = stream.getReader({ mode: 'byob' });
    // A minimum larger than the source's total: KJ tryRead semantics make the short read
    // the EOF signal, and the partial fill commits fused as {done: true, value: partial}.
    const { done, value } = await reader.read(new Uint8Array(64), { min: 20 });
    strictEqual(done, true);
    strictEqual(new TextDecoder().decode(value), 'hello world');
  },
};

export const nativeBackedAsyncIteration = {
  async test() {
    const decoder = new TextDecoder();
    let text = '';
    for await (const chunk of new Blob(['hello world']).stream()) {
      text += decoder.decode(chunk, { stream: true });
    }
    text += decoder.decode();
    strictEqual(text, 'hello world');
  },
};

export const nativeBackedCancel = {
  async test() {
    const stream = new Blob(['hello world']).stream();
    await stream.cancel('no longer interested');
    // Cancellation closes the stream; further reads observe EOF.
    const { done } = await stream.getReader().read();
    strictEqual(done, true);
  },
};

// Serves the fetch handlers backing the pumpTo and unwrap tests below. `/plain` responds
// with a buffer-backed body; `/proxy` forwards the fetched Response object UNMODIFIED, so
// its body -- a C++-created, TypeScript-backed stream held internally -- gets pumped by
// Response::send without JavaScript ever unwrapping it. That drives the native
// extraction pump path end to end over real HTTP. `/echo` reflects the request body back
// -- `request.body` is a TypeScript-implemented stream, so `new Response(request.body)`
// itself exercises jsgTryUnwrap's TS arm.
export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === '/plain') {
      return new Response('hello world');
    }
    if (url.pathname === '/proxy') {
      return env.SELF.fetch(new URL('/plain', request.url));
    }
    if (url.pathname === '/echo') {
      return new Response(request.body);
    }
    return new Response('not found', { status: 404 });
  },
};

export const nativeBackedPumpTo = {
  async test(ctrl, env) {
    const response = await env.SELF.fetch('http://example.org/proxy');
    strictEqual(response.status, 200);
    strictEqual(await response.text(), 'hello world');
  },
};

// C++ APIs accept TypeScript-implemented streams via jsgTryUnwrap's TS arm: a queued
// (plain JS underlying source) stream handed to Response is adopted by the C++ Body and
// consumed through the bridge consumers. Before the unwrap arm landed, this fell through
// to Body's async-iterable arm (TS streams are async iterable), so this also pins the
// dispatch: proper stream semantics, not chunk-by-chunk iteration.
export const tsStreamIntoResponse = {
  async test() {
    const encoder = new TextEncoder();
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(encoder.encode('hello '));
        c.enqueue(encoder.encode('world'));
        c.close();
      },
    });
    const response = new Response(rs);
    strictEqual(await response.text(), 'hello world');
  },
};

// The same over real HTTP: the queued stream becomes a fetch REQUEST body (Request's
// unwrap adopts it) and the outgoing send drives the queued drain-loop pump; the echo
// response body round-trips through unwrap a second time server-side.
export const tsStreamIntoFetchBody = {
  async test(ctrl, env) {
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(new TextEncoder().encode('ping'));
        c.close();
      },
    });
    const response = await env.SELF.fetch('http://example.org/echo', {
      method: 'POST',
      body: rs,
    });
    strictEqual(await response.text(), 'ping');
  },
};

// A native-backed TS stream round-trips INTO a C++ API: unwrap recognizes it like any
// other TS stream, and the outgoing send extracts the native source (pure-KJ pump)
// rather than draining through JS.
export const nativeBackedStreamIntoFetchBody = {
  async test(ctrl, env) {
    const response = await env.SELF.fetch('http://example.org/echo', {
      method: 'POST',
      body: new Blob(['native bytes']).stream(),
    });
    strictEqual(await response.text(), 'native bytes');
  },
};

// Body preconditions still apply after unwrap: a disturbed stream is rejected by the
// Body constructor itself (unwrap deliberately performs no such checks). This throw did
// NOT happen before the unwrap arm landed (the async-iterable fallback wrapped the
// stream in a fresh, undisturbed one), so this is a legacy-parity regression test.
export const disturbedTsStreamIntoResponse = {
  async test() {
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(new TextEncoder().encode('x'));
        c.close();
      },
    });
    const reader = rs.getReader();
    await reader.read();
    reader.releaseLock();
    throws(() => new Response(rs), {
      name: 'TypeError',
      message: /disturbed/,
    });
  },
};

// Non-stream objects keep their non-stream Body semantics: the brand check rejects them,
// the OneOf falls through, and a plain object stringifies per spec.
export const plainObjectBodyStillStringifies = {
  async test() {
    strictEqual(await new Response({}).text(), '[object Object]');
  },
};

export const nativeBackedTee = {
  async test() {
    const stream = new Blob(['hello world']).stream();
    // Native tee goes through the source's tee hook: the C++ side splits the underlying
    // source (via kj::newTee here -- the memory source has no optimized tee) and returns
    // two new native sources, each wrapped in a fully independent branch stream.
    const [a, b] = stream.tee();
    ok(stream.locked);
    ok(a instanceof ReadableStream);
    ok(b instanceof ReadableStream);

    async function readAll(s) {
      const decoder = new TextDecoder();
      let text = '';
      for await (const chunk of s) {
        text += decoder.decode(chunk, { stream: true });
      }
      return text + decoder.decode();
    }
    strictEqual(await readAll(a), 'hello world');
    strictEqual(await readAll(b), 'hello world');
  },
};
