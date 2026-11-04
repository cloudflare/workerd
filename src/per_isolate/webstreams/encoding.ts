'use strict';

// TextEncoderStream and TextDecoderStream — WHATWG Encoding Standard
// stream wrappers, implemented purely in JS/TS over the C++ codecs: the
// captured TextDecoder, and utils.encodeUtf8, which encodes non-Latin-1
// text with simdutf as the C++ TextEncoderStream does (TextEncoder.encode()
// does only under an autogate). These are standard category-3
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
import type { CommonHelpers } from './common';

const {
  ObjectDefineProperties,
  ObjectFreeze,
  StringPrototypeCharCodeAt,
  StringPrototypeSlice,
  SymbolToStringTag,
  TextDecoderEncodingGet,
  TextDecoder,
  TextDecoderFatalGet,
  TextDecoderIgnoreBOMGet,
  TextDecoderDecode,
  TypeError,
  getProtoGetter,
  uncurryThis,
} = primordials;

const { encodeUtf8, isArrayBuffer, isArrayBufferView, isSharedArrayBuffer } =
  utils;

const {
  TransformStream,
  TransformStreamDefaultController,
} = require('webstreams/transform');
const {
  isActualObject,
  kEnumerable,
  kNullProtoObject,
  TransformerRecord,
  toStringTagProperty,
} = require('webstreams/common') as CommonHelpers;

// TextDecoderStream's options for every chunk's decode, made once.
const kDecodeStreamOptions = ObjectFreeze({
  __proto__: kNullProtoObject,
  stream: true,
}) as TextDecodeOptions;

const transformControllerEnqueue = uncurryThis(
  TransformStreamDefaultController.prototype.enqueue
) as (controller: object, chunk: unknown) => void;

// Capture TransformStream.prototype accessors at bootstrap time so
// internal reads do not go through the (user-patchable) prototype chain.
const transformStreamReadableGet = getProtoGetter<
  (stream: object) => ReadableStreamType<unknown>
>(TransformStream.prototype, 'readable');
const transformStreamWritableGet = getProtoGetter<
  (stream: object) => WritableStreamType<unknown>
>(TransformStream.prototype, 'writable');

let assertIsTextEncoderStream: (self: TextEncoderStream) => void;
let assertIsTextDecoderStream: (self: TextDecoderStream) => void;

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

  static {
    assertIsTextEncoderStream = function (self: TextEncoderStream) {
      if (!isActualObject(self) || !(#transform in self))
        throw new TypeError('Illegal invocation');
    };
  }

  constructor() {
    const self = this;
    const transform = (chunk: unknown, controller: object): void => {
      // Spec: "encode and enqueue a chunk" ToString-coerces the input.
      // Template literal = ToString exactly: user toString honored for
      // objects, TypeError for symbols (String() would stringify them).
      const str = typeof chunk === 'string' ? chunk : `${chunk}`;
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
          transformControllerEnqueue(controller, encodeUtf8(prefix));
        }
      } else {
        self.#pendingHighSurrogate = '';
        transformControllerEnqueue(controller, encodeUtf8(text));
      }
    };
    const flush = (controller: object): void => {
      // A pending high surrogate at end-of-stream is replaced with
      // U+FFFD (the replacement character).
      if (self.#pendingHighSurrogate !== '') {
        transformControllerEnqueue(controller, encodeUtf8('\uFFFD'));
        self.#pendingHighSurrogate = '';
      }
    };
    this.#transform = new TransformStream(
      new TransformerRecord(
        undefined, // cancel
        undefined, // expectedLength
        flush, // flush
        undefined, // readableType
        undefined, // start
        transform, // transform
        undefined // writableType
      )
    );
  }

  get encoding(): string {
    assertIsTextEncoderStream(this);
    return 'utf-8';
  }

  get readable(): ReadableStreamType<Uint8Array> {
    assertIsTextEncoderStream(this);
    return transformStreamReadableGet(
      this.#transform
    ) as ReadableStreamType<Uint8Array>;
  }

  get writable(): WritableStreamType<string> {
    assertIsTextEncoderStream(this);
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

  static {
    assertIsTextDecoderStream = function (self: TextDecoderStream) {
      if (!isActualObject(self) || !(#decoder in self))
        throw new TypeError('Illegal invocation');
    };
  }

  constructor(label: string = 'utf-8', options?: TextDecoderStreamOptions) {
    const decoder = new TextDecoder(label, options);
    this.#decoder = decoder;

    const transform = (chunk: unknown, controller: object): void => {
      // The writable accepts an AllowSharedBufferSource (Encoding
      // Standard), so a bare SharedArrayBuffer decodes like an
      // ArrayBuffer.
      if (
        !isArrayBufferView(chunk) &&
        !isArrayBuffer(chunk) &&
        !isSharedArrayBuffer(chunk)
      ) {
        throw new TypeError('TextDecoderStream: chunk must be a BufferSource');
      }
      const decoded = TextDecoderDecode(
        decoder,
        chunk as BufferSource,
        kDecodeStreamOptions
      );
      if (decoded.length > 0) {
        transformControllerEnqueue(controller, decoded);
      }
    };
    const flush = (controller: object): void => {
      // Final decode: flushes any incomplete multi-byte sequences.
      // In fatal mode this throws if the sequence is incomplete.
      const decoded = TextDecoderDecode(decoder);
      if (decoded.length > 0) {
        transformControllerEnqueue(controller, decoded);
      }
    };
    this.#transform = new TransformStream(
      new TransformerRecord(
        undefined, // cancel
        undefined, // expectedLength
        flush, // flush
        undefined, // readableType
        undefined, // start
        transform, // transform
        undefined // writableType
      )
    );
  }

  get encoding(): string {
    assertIsTextDecoderStream(this);
    return TextDecoderEncodingGet(this.#decoder);
  }

  get fatal(): boolean {
    assertIsTextDecoderStream(this);
    return TextDecoderFatalGet(this.#decoder);
  }

  get ignoreBOM(): boolean {
    assertIsTextDecoderStream(this);
    return TextDecoderIgnoreBOMGet(this.#decoder);
  }

  get readable(): ReadableStreamType<string> {
    assertIsTextDecoderStream(this);
    return transformStreamReadableGet(
      this.#transform
    ) as ReadableStreamType<string>;
  }

  get writable(): WritableStreamType<BufferSource> {
    assertIsTextDecoderStream(this);
    return transformStreamWritableGet(
      this.#transform
    ) as WritableStreamType<BufferSource>;
  }
}

ObjectDefineProperties(TextEncoderStream.prototype, {
  __proto__: kNullProtoObject,
  encoding: kEnumerable,
  readable: kEnumerable,
  writable: kEnumerable,
  [SymbolToStringTag]: toStringTagProperty('TextEncoderStream'),
});

ObjectDefineProperties(TextDecoderStream.prototype, {
  __proto__: kNullProtoObject,
  encoding: kEnumerable,
  fatal: kEnumerable,
  ignoreBOM: kEnumerable,
  readable: kEnumerable,
  writable: kEnumerable,
  [SymbolToStringTag]: toStringTagProperty('TextDecoderStream'),
});

module.exports = {
  TextEncoderStream,
  TextDecoderStream,
};
