'use strict';

// TextEncoderStream and TextDecoderStream — WHATWG Encoding Standard
// stream wrappers, implemented purely in JS/TS over the captured
// TextEncoder/TextDecoder primitives. These are standard category-3
// transforms (not elidable, not native-backed): the codec work
// dominates, and the readable side yields values (strings for TDS,
// Uint8Array for TES), not bytes — so they deliberately do NOT use
// the native-backed byte-stream path.
//
// The C++ stream wrappers these replace are legacy artifacts predating
// the per-isolate bootstrap; the codec primitives stay C++.

import type {
  ReadableStream as ReadableStreamType,
  WritableStream as WritableStreamType,
} from './types';

const {
  ObjectDefineProperties,
  ObjectGetOwnPropertyDescriptor,
  StringPrototypeCharCodeAt,
  StringPrototypeSlice,
  textDecoderEncodingGet,
  TextDecoder,
  TextEncoder,
  textDecoderFatalGet,
  textDecoderIgnoreBOMGet,
  textEncoderEncode,
  textDecoderDecode,
  TypeError,
  uncurryThis,
} = primordials;

const { isArrayBuffer, isArrayBufferView } = utils;

// Captured for primordials discipline — ToString coercion per spec.
const StringCoerce = String;

const { TransformStream } = require('webstreams/transform');

// Capture TransformStream.prototype accessors at bootstrap time so
// internal reads do not go through the (user-patchable) prototype chain.
// Uses the same validation guard pattern as getProtoGetter in primordials.ts.
function getProtoGetter<T>(proto: object, name: string): T {
  const desc = ObjectGetOwnPropertyDescriptor(proto, name);
  if (desc === undefined || desc.get === undefined) {
    throw new TypeError(`Expected accessor property '${name}' on prototype`);
  }
  return uncurryThis(desc.get) as T;
}

const transformStreamReadableGet = getProtoGetter<
  (stream: object) => ReadableStreamType<unknown>
>(TransformStream.prototype, 'readable');
const transformStreamWritableGet = getProtoGetter<
  (stream: object) => WritableStreamType<unknown>
>(TransformStream.prototype, 'writable');

// ---------------------------------------------------------------------------
// TextEncoderStream
//
// Spec: https://encoding.spec.whatwg.org/#interface-textencoderstream
//
// Encodes strings to UTF-8. Stateful: a lone high surrogate at the end
// of a chunk is held and paired with the next chunk's leading low
// surrogate (spec §10.1.1 "encode and enqueue a chunk"). If the stream
// closes with a pending high surrogate, it's replaced with U+FFFD.

class TextEncoderStream {
  #transform: InstanceType<typeof TransformStream>;
  #pendingHighSurrogate: string = '';

  constructor() {
    const self = this;
    const encoder = new TextEncoder();
    this.#transform = new TransformStream({
      transform(
        chunk: unknown,
        controller: { enqueue: (c: Uint8Array) => void }
      ) {
        // Spec: "encode and enqueue a chunk" ToString-coerces the input.
        const str = typeof chunk === 'string' ? chunk : StringCoerce(chunk);
        const text = self.#pendingHighSurrogate + str;
        const len = text.length;
        if (len === 0) return;

        // Check if the last code unit is a lone high surrogate
        // (0xD800–0xDBFF). If so, hold it for the next chunk.
        const last = StringPrototypeCharCodeAt(text, len - 1) as number;
        if (last >= 0xd800 && last <= 0xdbff) {
          self.#pendingHighSurrogate = StringPrototypeSlice(
            text,
            len - 1
          ) as string;
          const prefix = StringPrototypeSlice(text, 0, len - 1) as string;
          if (prefix.length > 0) {
            controller.enqueue(textEncoderEncode(encoder, prefix));
          }
        } else {
          self.#pendingHighSurrogate = '';
          controller.enqueue(textEncoderEncode(encoder, text));
        }
      },
      flush(controller: { enqueue: (c: Uint8Array) => void }) {
        // A pending high surrogate at end-of-stream is replaced with
        // U+FFFD (the replacement character).
        if (self.#pendingHighSurrogate !== '') {
          controller.enqueue(textEncoderEncode(encoder, '\uFFFD'));
          self.#pendingHighSurrogate = '';
        }
      },
    });
  }

  get encoding(): string {
    return 'utf-8';
  }

  get readable(): ReadableStreamType<Uint8Array> {
    if (!(#transform in this)) throw new TypeError('Illegal invocation');
    return transformStreamReadableGet(
      this.#transform
    ) as ReadableStreamType<Uint8Array>;
  }

  get writable(): WritableStreamType<string> {
    if (!(#transform in this)) throw new TypeError('Illegal invocation');
    return transformStreamWritableGet(
      this.#transform
    ) as WritableStreamType<string>;
  }
}

// ---------------------------------------------------------------------------
// TextDecoderStream
//
// Spec: https://encoding.spec.whatwg.org/#interface-textdecoderstream
//
// Decodes bytes to strings. Wraps a stateful TextDecoder with
// {stream: true} in transform and a final decode() in flush to catch
// incomplete multi-byte sequences.

interface TextDecoderStreamOptions {
  fatal?: boolean;
  ignoreBOM?: boolean;
}

class TextDecoderStream {
  #transform: InstanceType<typeof TransformStream>;
  #decoder: TextDecoder;

  constructor(label: string = 'utf-8', options?: TextDecoderStreamOptions) {
    const decoder = new TextDecoder(label, options);
    this.#decoder = decoder;

    this.#transform = new TransformStream({
      transform(chunk: unknown, controller: { enqueue: (c: string) => void }) {
        if (!isArrayBufferView(chunk) && !isArrayBuffer(chunk)) {
          throw new TypeError(
            'TextDecoderStream: chunk must be a BufferSource'
          );
        }
        const decoded = textDecoderDecode(decoder, chunk as BufferSource, {
          stream: true,
        });
        if (decoded.length > 0) {
          controller.enqueue(decoded);
        }
      },
      flush(controller: { enqueue: (c: string) => void }) {
        // Final decode: flushes any incomplete multi-byte sequences.
        // In fatal mode this throws if the sequence is incomplete.
        const decoded = textDecoderDecode(decoder);
        if (decoded.length > 0) {
          controller.enqueue(decoded);
        }
      },
    });
  }

  get encoding(): string {
    if (!(#decoder in this)) throw new TypeError('Illegal invocation');
    return textDecoderEncodingGet(this.#decoder);
  }

  get fatal(): boolean {
    if (!(#decoder in this)) throw new TypeError('Illegal invocation');
    return textDecoderFatalGet(this.#decoder);
  }

  get ignoreBOM(): boolean {
    if (!(#decoder in this)) throw new TypeError('Illegal invocation');
    return textDecoderIgnoreBOMGet(this.#decoder);
  }

  get readable(): ReadableStreamType<string> {
    if (!(#transform in this)) throw new TypeError('Illegal invocation');
    return transformStreamReadableGet(
      this.#transform
    ) as ReadableStreamType<string>;
  }

  get writable(): WritableStreamType<BufferSource> {
    if (!(#transform in this)) throw new TypeError('Illegal invocation');
    return transformStreamWritableGet(
      this.#transform
    ) as WritableStreamType<BufferSource>;
  }
}

ObjectDefineProperties(TextEncoderStream.prototype, {
  encoding: { enumerable: true },
  readable: { enumerable: true },
  writable: { enumerable: true },
});

ObjectDefineProperties(TextDecoderStream.prototype, {
  encoding: { enumerable: true },
  fatal: { enumerable: true },
  ignoreBOM: { enumerable: true },
  readable: { enumerable: true },
  writable: { enumerable: true },
});

module.exports = {
  TextEncoderStream,
  TextDecoderStream,
};
