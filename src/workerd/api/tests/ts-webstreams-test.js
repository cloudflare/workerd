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

import { doesNotMatch, ok, strictEqual } from 'node:assert';

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
