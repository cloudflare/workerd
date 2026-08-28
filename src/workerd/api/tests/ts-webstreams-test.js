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

import {
  deepStrictEqual,
  doesNotMatch,
  ok,
  rejects,
  strictEqual,
} from 'node:assert';

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

// ======================================================================================
// Buffer-backed bodies (Body::extractBody's in-memory arms: strings, ArrayBuffers,
// typed arrays, Blobs, URLSearchParams) construct their streams through the same
// compat-flag dispatch as every other C++-minted stream, so under the flag
// request/response bodies are TypeScript-implemented streams over the shared in-memory
// buffer.

export const bufferBackedBodiesAreTsStreams = {
  async test() {
    const bodies = [
      'hello world',
      new TextEncoder().encode('hello world'),
      new TextEncoder().encode('hello world').buffer,
      new Blob(['hello world']),
    ];
    for (const body of bodies) {
      const response = new Response(body);
      // The body stream is an instance of the TypeScript-implemented class (a legacy
      // C++ stream would fail this brand check)...
      ok(response.body instanceof ReadableStream);
      // ...and the C++ consumption helpers drain it through the TS conduit.
      strictEqual(await response.text(), 'hello world');
    }
  },
};

export const bufferBackedRequestBody = {
  async test() {
    const request = new Request('http://example.org/', {
      method: 'POST',
      body: 'hello world',
    });
    ok(request.body instanceof ReadableStream);
    strictEqual(await request.text(), 'hello world');
  },
};

export const bufferBackedBodyReaderRead = {
  async test() {
    // Reading a buffer-backed body from JavaScript drives the TS reader machinery over
    // the native in-memory source.
    const response = new Response('hello world');
    const reader = response.body.getReader();
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

export const bufferBackedUrlSearchParamsBody = {
  async test() {
    const response = new Response(new URLSearchParams({ a: '1', b: '2' }));
    ok(response.body instanceof ReadableStream);
    strictEqual(await response.text(), 'a=1&b=2');
  },
};

export const bufferBackedEmptyBody = {
  async test() {
    // An empty buffer-backed body closes immediately (expectedLength 0).
    const response = new Response('');
    ok(response.body instanceof ReadableStream);
    strictEqual(await response.text(), '');
  },
};

// ======================================================================================
// Iterable/AsyncIterable bodies (the fetch_iterable_type_support BodyInit extension) go
// through JsReadableStream::from(), which under the flag constructs a TypeScript stream
// over a C++-built underlying source driving the (already-unwrapped) generator with
// ReadableStream.from() semantics: demand-driven pulls, promise-typed values awaited,
// close on completion, cancel forwarded to the iterator's return().

export const iterableBodiesAreTsStreams = {
  async test() {
    const encoder = new TextEncoder();

    // Async generator.
    async function* agen() {
      yield encoder.encode('hello ');
      yield encoder.encode('world');
    }
    let response = new Response(agen());
    ok(response.body instanceof ReadableStream);
    strictEqual(await response.text(), 'hello world');

    // Sync iterable object.
    response = new Response({
      *[Symbol.iterator]() {
        yield encoder.encode('hello ');
        yield encoder.encode('world');
      },
    });
    ok(response.body instanceof ReadableStream);
    strictEqual(await response.text(), 'hello world');

    // Promise-typed values are awaited before being enqueued (async-from-sync
    // semantics, matching the legacy implementation).
    response = new Response({
      *[Symbol.iterator]() {
        yield Promise.resolve(encoder.encode('hello '));
        yield Promise.resolve(encoder.encode('world'));
      },
    });
    strictEqual(await response.text(), 'hello world');
  },
};

export const iterableBodyReaderRead = {
  async test() {
    // Reading the iterable-backed body from JavaScript drives the TS reader machinery:
    // one generator value per read, EOF after completion.
    async function* agen() {
      yield new TextEncoder().encode('hello world');
    }
    const response = new Response(agen());
    const reader = response.body.getReader();
    const first = await reader.read();
    strictEqual(first.done, false);
    strictEqual(new TextDecoder().decode(first.value), 'hello world');
    const eof = await reader.read();
    strictEqual(eof.done, true);
  },
};

// Internal stream production (ReadableStream.from and the iterable-body path,
// whose C++ arm drives the same queued controller) must not dispatch through
// the user-patchable ReadableStreamDefaultController prototype: per WHATWG,
// from() uses internal controller operations, so patched enqueue/close must
// have no effect on either surface.
export const controllerPrototypePollution = {
  async test() {
    const proto = ReadableStreamDefaultController.prototype;
    const origEnqueue = proto.enqueue;
    const origClose = proto.close;
    const trapped = [];
    proto.enqueue = function (...args) {
      trapped.push('enqueue');
      return origEnqueue.apply(this, args);
    };
    proto.close = function (...args) {
      trapped.push('close');
      return origClose.apply(this, args);
    };
    try {
      // ReadableStream.from (TS-internal production).
      {
        const stream = ReadableStream.from(['pol', 'lution', '-proof']);
        const reader = stream.getReader();
        let text = '';
        while (true) {
          const { value, done } = await reader.read();
          if (done) break;
          text += value;
        }
        strictEqual(text, 'pollution-proof');
      }

      // Iterable body (the C++ from() arm driving the same controller).
      {
        async function* agen() {
          yield new TextEncoder().encode('body intact');
        }
        strictEqual(await new Response(agen()).text(), 'body intact');
      }

      deepStrictEqual(
        trapped,
        [],
        `internal production dispatched through patched prototype: ${trapped.join(', ')}`
      );
    } finally {
      proto.enqueue = origEnqueue;
      proto.close = origClose;
    }
  },
};

export const iterableBodyCancelCallsReturn = {
  async test() {
    // Canceling the body forwards to the iterator's return() hook.
    let returned = false;
    const response = new Response({
      [Symbol.asyncIterator]() {
        return {
          next() {
            return Promise.resolve({
              value: new TextEncoder().encode('x'),
              done: false,
            });
          },
          return() {
            returned = true;
            return Promise.resolve({ done: true });
          },
        };
      },
    });
    await response.body.cancel('no longer interested');
    ok(returned);
  },
};

export const iterableBodyErrorPropagates = {
  async test() {
    // A generator throw rejects the pull, which errors the stream and the consumption.
    async function* agen() {
      yield new TextEncoder().encode('x');
      throw new Error('boom');
    }
    const response = new Response(agen());
    await rejects(response.text(), /boom/);
  },
};

// ======================================================================================
// new Request(existingRequest) proxies the body by detaching it: the original request's
// stream is taken over (left permanently locked and disturbed) and the new request reads
// the data. Under the flag this drives JsReadableStream::detach()'s TypeScript arm.

export const requestBodyProxying = {
  async test() {
    const req1 = new Request('http://example.org/', {
      method: 'POST',
      body: 'hello world',
    });
    const req2 = new Request(req1);
    ok(req2.body instanceof ReadableStream);
    // The original's body has been taken over: disturbed (bodyUsed) and locked.
    strictEqual(req1.bodyUsed, true);
    ok(req1.body.locked);
    strictEqual(await req2.text(), 'hello world');
  },
};

export const requestStreamBodyProxying = {
  async test() {
    const encoder = new TextEncoder();
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(encoder.encode('hello '));
        c.enqueue(encoder.encode('world'));
        c.close();
      },
    });
    const req1 = new Request('http://example.org/', {
      method: 'POST',
      body: rs,
    });
    const req2 = new Request(req1);
    // The adopted user stream is the one left as the locked husk...
    ok(rs.locked);
    // ...while the new request reads its (queued) data through the moved cursor.
    strictEqual(await req2.text(), 'hello world');
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

// Non-stream objects keep their non-stream Body semantics: the brand check rejects them,
// the OneOf falls through, and a plain object stringifies per spec.
export const plainObjectBodyStillStringifies = {
  async test() {
    strictEqual(await new Response({}).text(), '[object Object]');
  },
};

// The same for a native-backed body: the fetched response's body is a C++-created,
// TypeScript-backed stream, so clone() routes C++ tee -> TS tee -> the native tee hook
// (ReadableStreamNativeSource::tee).
export const nativeBackedBodyClone = {
  async test(ctrl, env) {
    const response = await env.SELF.fetch('http://example.org/plain');
    const clone = response.clone();
    const [a, b] = await Promise.all([response.text(), clone.text()]);
    strictEqual(a, 'hello world');
    strictEqual(b, 'hello world');
  },
};

// Request.clone() takes the same path as Response.clone().
export const tsStreamRequestClone = {
  async test() {
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(new TextEncoder().encode('request body'));
        c.close();
      },
    });
    const request = new Request('http://example.org/', {
      method: 'POST',
      body: rs,
    });
    const clone = request.clone();
    const [a, b] = await Promise.all([request.text(), clone.text()]);
    strictEqual(a, 'request body');
    strictEqual(b, 'request body');
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
