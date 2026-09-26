// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Prototype pollution the streams machinery must not observe: a patched
// array iterator, a replaced Number global, patched controller prototype
// methods, and Object.prototype members standing in for omitted dictionary
// arguments. The C++ implementation reads none of these; the TypeScript one
// must not either. Every mutation is undone before any assertion runs.
//
// Divergence: @@asyncIterator is the values() function object in TS (WebIDL);
// C++ installs a separate function.

import { strictEqual, deepStrictEqual, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

function helloBody() {
  return new ReadableStream({
    start(c) {
      c.enqueue(new TextEncoder().encode('hello'));
      c.close();
    },
  });
}

// A patched %ArrayIteratorPrototype%.next must not empty a body.
export const patchedArrayIteratorKeepsBody = {
  async test() {
    const iteratorProto = Object.getPrototypeOf([][Symbol.iterator]());
    const saved = iteratorProto.next;
    iteratorProto.next = function () {
      return { done: true, value: undefined };
    };
    let text;
    let bytes;
    try {
      text = await new Response(helloBody()).text();
      bytes = await new Response(helloBody()).arrayBuffer();
    } finally {
      iteratorProto.next = saved;
    }
    strictEqual(text, 'hello');
    strictEqual(bytes.byteLength, 5);
  },
};

// A replaced Number global must not change body sizes.
export const replacedNumberKeepsBody = {
  async test() {
    const saved = globalThis.Number;
    globalThis.Number = function () {
      return NaN;
    };
    let bytes;
    let text;
    try {
      bytes = await new Response(helloBody()).arrayBuffer();
      text = await new Response(helloBody()).text();
    } finally {
      globalThis.Number = saved;
    }
    strictEqual(bytes.byteLength, 5);
    strictEqual(text, 'hello');
  },
};

// A patched controller error() must not stop a pull rejection from
// erroring the stream.
export const patchedControllerErrorStillErrors = {
  async test() {
    const proto = ReadableStreamDefaultController.prototype;
    const saved = proto.error;
    let calls = 0;
    proto.error = function () {
      calls++;
    };
    let outcome;
    try {
      const rs = new ReadableStream({
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

// An omitted dictionary argument is an empty dictionary: nothing is read
// from Object.prototype for it.
export const omittedDictionariesReadNothing = {
  async test() {
    let pollutedStarts = 0;
    const pollution = {
      type: 'bytes',
      start() {
        pollutedStarts++;
      },
      size() {
        return 100;
      },
      highWaterMark: 7,
      mode: 'byob',
      preventCancel: true,
      preventClose: true,
      signal: 'not a signal',
    };
    for (const key of Object.keys(pollution)) {
      Object.defineProperty(Object.prototype, key, {
        value: pollution[key],
        configurable: true,
        writable: true,
      });
    }
    let desiredBefore;
    let desiredAfter;
    let readerName;
    let cancelled = false;
    let closed = false;
    try {
      // Constructor with both arguments omitted.
      const bare = new ReadableStream();
      readerName = bare.getReader().constructor.name;

      // Omitted strategy: default high-water mark and size.
      let controller;
      const rs = new ReadableStream({
        __proto__: null,
        start(c) {
          controller = c;
        },
        cancel() {
          cancelled = true;
        },
      });
      desiredBefore = controller.desiredSize;
      controller.enqueue('x');
      desiredAfter = controller.desiredSize;

      // Omitted iterator options: return() cancels.
      await rs.values().return();

      // Omitted pipe options: no signal, and the destination closes.
      await new ReadableStream({
        __proto__: null,
        start(c) {
          c.enqueue('y');
          c.close();
        },
      }).pipeTo(
        new WritableStream({
          __proto__: null,
          close() {
            closed = true;
          },
        })
      );
    } finally {
      for (const key of Object.keys(pollution)) {
        Reflect.deleteProperty(Object.prototype, key);
      }
    }
    strictEqual('type' in {}, false, 'pollution must be removed');
    strictEqual(pollutedStarts, 0);
    strictEqual(readerName, 'ReadableStreamDefaultReader');
    strictEqual(desiredBefore, 1);
    strictEqual(desiredAfter, 0);
    strictEqual(cancelled, true);
    strictEqual(closed, true);
  },
};

// ReadableStream.from() builds its source and strategy itself: nothing is
// read from Object.prototype for them.
export const fromBuildsNoDictionariesFromObjectPrototype = {
  async test() {
    let pollutedCalls = 0;
    const pollution = {
      type: 'bytes',
      start() {
        pollutedCalls++;
      },
      size() {
        pollutedCalls++;
        return 100;
      },
      highWaterMark: 7,
      autoAllocateChunkSize: 16,
    };
    for (const key of Object.keys(pollution)) {
      Object.defineProperty(Object.prototype, key, {
        value: pollution[key],
        configurable: true,
        writable: true,
      });
    }
    const results = [];
    try {
      async function* gen() {
        yield 'c';
      }
      for (const rs of [
        ReadableStream.from(['a', 'b']),
        ReadableStream.from(gen()),
        ReadableStream.from('s'),
      ]) {
        const reader = rs.getReader();
        const chunks = [];
        for (;;) {
          const { value, done } = await reader.read();
          if (done) break;
          chunks.push(value);
        }
        results.push(chunks);
      }
    } finally {
      for (const key of Object.keys(pollution)) {
        Reflect.deleteProperty(Object.prototype, key);
      }
    }
    strictEqual('type' in {}, false, 'pollution must be removed');
    strictEqual(pollutedCalls, 0);
    deepStrictEqual(results, [['a', 'b'], ['c'], ['s']]);
  },
};

// @@asyncIterator is not enumerable, and patching values() does not reach
// for-await.
export const asyncIteratorShape = {
  async test() {
    const desc = Object.getOwnPropertyDescriptor(
      ReadableStream.prototype,
      Symbol.asyncIterator
    );
    strictEqual(desc.enumerable, false);
    strictEqual(desc.writable, true);
    strictEqual(desc.configurable, true);
    strictEqual(desc.value === ReadableStream.prototype.values, usingTsImpl);

    const saved = ReadableStream.prototype.values;
    ReadableStream.prototype.values = function () {
      throw new Error('patched');
    };
    const chunks = [];
    try {
      const rs = new ReadableStream({
        start(c) {
          c.enqueue('a');
          c.enqueue('b');
          c.close();
        },
      });
      for await (const chunk of rs) {
        chunks.push(chunk);
      }
    } finally {
      ReadableStream.prototype.values = saved;
    }
    deepStrictEqual(chunks, ['a', 'b']);
  },
};
