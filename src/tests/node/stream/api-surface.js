// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The web-interop surface of node:stream: node:stream/web re-exports the
// global Web Streams classes by identity, and the adapter entry points hang
// off the node:stream classes and their legacy node:_stream_* aliases.

import * as web from 'node:stream/web';
import webDefault from 'node:stream/web';
import { Readable, Writable, Duplex } from 'node:stream';
import { Readable as LegacyReadable } from 'node:_stream_readable';
import { Writable as LegacyWritable } from 'node:_stream_writable';
import { Duplex as LegacyDuplex } from 'node:_stream_duplex';
import { strictEqual, deepStrictEqual } from 'node:assert';

const WEB_CLASSES = [
  'ReadableStream',
  'ReadableStreamDefaultReader',
  'ReadableStreamBYOBReader',
  'ReadableStreamBYOBRequest',
  'ReadableByteStreamController',
  'ReadableStreamDefaultController',
  'TransformStream',
  'TransformStreamDefaultController',
  'WritableStream',
  'WritableStreamDefaultWriter',
  'WritableStreamDefaultController',
  'ByteLengthQueuingStrategy',
  'CountQueuingStrategy',
  'TextEncoderStream',
  'TextDecoderStream',
  'CompressionStream',
  'DecompressionStream',
];

// Every node:stream/web named export is the same object as the global of
// the same name, under either implementation, and the default export
// carries exactly that set.
export const streamWebReexportsGlobals = {
  test() {
    for (const name of WEB_CLASSES) {
      strictEqual(typeof globalThis[name], 'function', name);
      strictEqual(web[name], globalThis[name], name);
      strictEqual(webDefault[name], globalThis[name], `default.${name}`);
    }
    deepStrictEqual(Object.keys(webDefault).sort(), [...WEB_CLASSES].sort());
  },
};

// The adapters are static functions on the classes, shared with the legacy
// aliases.
export const adapterEntryPoints = {
  test() {
    for (const [cls, legacy] of [
      [Readable, LegacyReadable],
      [Writable, LegacyWritable],
      [Duplex, LegacyDuplex],
    ]) {
      strictEqual(cls, legacy);
      strictEqual(typeof cls.toWeb, 'function');
      strictEqual(typeof cls.fromWeb, 'function');
    }
    // Duplex.toWeb / fromWeb are its own functions, not the Readable or
    // Writable ones inherited through the prototype chain.
    strictEqual(Object.hasOwn(Duplex, 'toWeb'), true);
    strictEqual(Object.hasOwn(Duplex, 'fromWeb'), true);
  },
};
