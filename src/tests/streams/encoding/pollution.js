// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Prototype pollution the encoding streams must not observe: a patched
// TransformStreamDefaultController enqueue(), and Object.prototype members
// reaching the internal transformer dictionaries. The C++ implementation
// reads none of these; the TypeScript one must not either. Every mutation
// is undone before any assertion runs.

import { strictEqual } from 'node:assert';

function pollute(entries) {
  for (const key of Object.keys(entries)) {
    Object.defineProperty(Object.prototype, key, {
      value: entries[key],
      configurable: true,
      writable: true,
    });
  }
  return () => {
    for (const key of Object.keys(entries)) {
      Reflect.deleteProperty(Object.prototype, key);
    }
  };
}

async function decodeThrough(transform, bytes) {
  const writer = transform.writable.getWriter();
  const done = writer.write(bytes).then(() => writer.close());
  const reader = transform.readable.getReader();
  let text = '';
  for (;;) {
    const { value, done: end } = await reader.read();
    if (end) break;
    text += value;
  }
  await done;
  return text;
}

async function encodeThrough(transform, text) {
  const writer = transform.writable.getWriter();
  const done = writer.write(text).then(() => writer.close());
  const bytes = await new Response(transform.readable).arrayBuffer();
  await done;
  return new TextDecoder().decode(bytes);
}

// A patched controller enqueue() must not rewrite TextDecoderStream or
// TextEncoderStream output.
export const patchedControllerEnqueueDoesNotRewriteOutput = {
  async test() {
    const proto = TransformStreamDefaultController.prototype;
    const saved = proto.enqueue;
    let calls = 0;
    proto.enqueue = function (chunk) {
      calls++;
      return saved.call(this, typeof chunk === 'string' ? 'POLLUTED' : chunk);
    };
    let decoded;
    let encoded;
    try {
      decoded = await decodeThrough(
        new TextDecoderStream(),
        new TextEncoder().encode('hi')
      );
      encoded = await encodeThrough(new TextEncoderStream(), 'hi');
    } finally {
      proto.enqueue = saved;
    }
    strictEqual(calls, 0);
    strictEqual(decoded, 'hi');
    strictEqual(encoded, 'hi');
  },
};

export const internalDictionariesIgnorePollution = {
  async test() {
    let pollutedCalls = 0;
    const restore = pollute({
      type: 'bytes',
      readableType: 'bytes',
      writableType: 'bytes',
      start() {
        pollutedCalls++;
      },
      cancel() {
        pollutedCalls++;
      },
      size() {
        return 100;
      },
      highWaterMark: 7,
    });
    let decoded;
    let encoded;
    try {
      decoded = await decodeThrough(
        new TextDecoderStream(),
        new TextEncoder().encode('hi')
      );
      encoded = await encodeThrough(new TextEncoderStream(), 'hi');
    } finally {
      restore();
    }
    strictEqual('type' in {}, false, 'pollution must be removed');
    strictEqual(pollutedCalls, 0);
    strictEqual(decoded, 'hi');
    strictEqual(encoded, 'hi');
  },
};
