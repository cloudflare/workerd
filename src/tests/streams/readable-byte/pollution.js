// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Prototype pollution the byte-stream machinery must not observe: a
// patched byte controller error(), a patched BYOB reader read(), an
// ArrayBuffer species constructor, and Object.prototype members on a native
// body's C++ source. The C++ implementation reads none of these; the
// TypeScript one must not either. Every mutation is undone before any
// assertion runs.

import { strictEqual, deepStrictEqual, rejects } from 'node:assert';

// A patched controller error() must not stop a pull rejection from
// erroring the stream.
export const patchedByteControllerErrorStillErrors = {
  async test() {
    const proto = ReadableByteStreamController.prototype;
    const saved = proto.error;
    let calls = 0;
    proto.error = function () {
      calls++;
    };
    let outcome;
    try {
      const rs = new ReadableStream({
        type: 'bytes',
        pull() {
          return Promise.reject(new Error('boom'));
        },
      });
      const reader = rs.getReader();
      // Bounded: a read that pends would otherwise leave the patch in place
      // for the tests that follow.
      outcome = await Promise.race([
        rejects(reader.read(), { message: 'boom' }).then(() =>
          rejects(reader.closed, { message: 'boom' })
        ),
        scheduler.wait(500).then(() => 'read pended'),
      ]);
    } finally {
      proto.error = saved;
    }
    strictEqual(outcome, undefined);
    strictEqual(calls, 0);
  },
};

// readAtLeast() does not dispatch through the reader's read().
export const readAtLeastIgnoresPatchedRead = {
  async test() {
    const proto = ReadableStreamBYOBReader.prototype;
    const saved = proto.read;
    proto.read = function () {
      throw new Error('patched');
    };
    let result;
    try {
      const rs = new ReadableStream({
        type: 'bytes',
        start(c) {
          c.enqueue(new Uint8Array([1, 2, 3, 4]));
          c.close();
        },
      });
      result = await rs
        .getReader({ mode: 'byob' })
        .readAtLeast(4, new Uint8Array(4));
    } finally {
      proto.read = saved;
    }
    strictEqual(result.done, false);
    deepStrictEqual([...result.value], [1, 2, 3, 4]);
  },
};

// Internal byte copies (a tee branch's copy of a shared chunk, the
// unaligned remainder of a BYOB respond()) never consult the ArrayBuffer or
// %TypedArray% species constructors.
export const speciesNotConsultedByInternalCopies = {
  async test() {
    const TypedArray = Object.getPrototypeOf(Uint8Array);
    const saved = [
      [
        ArrayBuffer,
        Object.getOwnPropertyDescriptor(ArrayBuffer, Symbol.species),
      ],
      [TypedArray, Object.getOwnPropertyDescriptor(TypedArray, Symbol.species)],
    ];
    let calls = 0;
    for (const [ctor] of saved) {
      Object.defineProperty(ctor, Symbol.species, {
        get() {
          calls++;
          return ctor;
        },
        configurable: true,
      });
    }
    let teed;
    let aligned;
    let remainder;
    try {
      // Tee: the first branch reads while the second still needs the
      // chunk, so it receives a copy.
      const [a, b] = new ReadableStream({
        type: 'bytes',
        start(c) {
          c.enqueue(new Uint8Array([1, 2, 3, 4]));
          c.close();
        },
      }).tee();
      const fromA = await a.getReader().read();
      const fromB = await b.getReader().read();
      teed = [Array.from(fromA.value), Array.from(fromB.value)];

      // respond(3) into a Uint16Array view: one element is delivered and
      // the odd byte is copied back into the queue.
      let controller;
      const rs = new ReadableStream({
        type: 'bytes',
        start(c) {
          controller = c;
        },
      });
      const reader = rs.getReader({ mode: 'byob' });
      const pending = reader.read(new Uint16Array(2));
      new Uint8Array(controller.byobRequest.view.buffer).set([5, 6, 7], 0);
      controller.byobRequest.respond(3);
      const first = (await pending).value;
      aligned = Array.from(
        new Uint8Array(first.buffer, first.byteOffset, first.byteLength)
      );
      remainder = Array.from((await reader.read(new Uint8Array(1))).value);
    } finally {
      for (const [ctor, desc] of saved) {
        Object.defineProperty(ctor, Symbol.species, desc);
      }
    }
    strictEqual(calls, 0);
    deepStrictEqual(teed, [
      [1, 2, 3, 4],
      [1, 2, 3, 4],
    ]);
    deepStrictEqual(aligned, [5, 6]);
    deepStrictEqual(remainder, [7]);
  },
};

// A native body's C++ source declares neither type nor
// autoAllocateChunkSize; Object.prototype must not supply them.
export const nativeSourceIgnoresPollutedMembers = {
  async test() {
    Object.defineProperty(Object.prototype, 'type', {
      value: 'bytes',
      configurable: true,
      writable: true,
    });
    Object.defineProperty(Object.prototype, 'autoAllocateChunkSize', {
      value: 16,
      configurable: true,
      writable: true,
    });
    let body;
    let result;
    try {
      body = new Response('hello').body;
      result = await body.getReader().read();
    } finally {
      delete Object.prototype.type;
      delete Object.prototype.autoAllocateChunkSize;
    }
    strictEqual('type' in {}, false, 'pollution must be removed');
    strictEqual(result.done, false);
    strictEqual(new TextDecoder().decode(result.value), 'hello');
  },
};
