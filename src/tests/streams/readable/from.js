// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// ReadableStream.from(): iterable/async-iterable adoption and cancel
// plumbing through the iterator protocol. Migrated from streams-test.js;
// then the iterator protocol's observable steps (WebIDL async_sequence and
// ECMA-262's async-from-sync iterator), the string and ArrayBufferView
// divergences, and the exact rejection messages.

import { strictEqual, deepStrictEqual, rejects, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { drainToArray, rejectionOf } from 'helpers';

export const readableStreamFromAsyncGenerator = {
  async test() {
    async function* gen() {
      await scheduler.wait(10);
      yield 'hello';
      await scheduler.wait(10);
      yield 'world';
    }
    const rs = ReadableStream.from(gen());
    const chunks = [];
    for await (const chunk of rs) {
      chunks.push(chunk);
    }
    deepStrictEqual(chunks, ['hello', 'world']);
  },
};

export const readableStreamFromSyncGenerator = {
  async test() {
    const rs = ReadableStream.from(['hello', 'world']);
    const chunks = [];
    for await (const chunk of rs) {
      chunks.push(chunk);
    }
    deepStrictEqual(chunks, ['hello', 'world']);
  },
};

export const readableStreamFromSyncGenerator2 = {
  async test() {
    function* gen() {
      yield 'hello';
      yield 'world';
    }
    const rs = ReadableStream.from(gen());
    const chunks = [];
    for await (const chunk of rs) {
      chunks.push(chunk);
    }
    deepStrictEqual(chunks, ['hello', 'world']);
  },
};

export const readableStreamFromAsyncCanceled = {
  async test() {
    async function* gen() {
      let count = 0;
      try {
        count++;
        yield 'hello';
        count++;
        yield 'world';
      } finally {
        strictEqual(count, 1);
      }
    }
    const rs = ReadableStream.from(gen());
    const chunks = [];
    for await (const chunk of rs) {
      chunks.push(chunk);
      return;
    }
    deepStrictEqual(chunks, ['hello']);
  },
};

export const readableStreamFromThrowingAsyncGen = {
  async test() {
    async function* gen() {
      yield 'hello';
      throw new Error('boom');
    }
    const rs = ReadableStream.from(gen());
    const chunks = [];
    async function consumeStream() {
      for await (const chunk of rs) {
        chunks.push(chunk);
      }
    }
    await rejects(consumeStream, { message: 'boom' });
    deepStrictEqual(chunks, ['hello']);
  },
};

export const readableStreamFromNoopAsyncGen = {
  async test() {
    async function* gen() {}
    const rs = ReadableStream.from(gen());
    const chunks = [];
    for await (const chunk of rs) {
      chunks.push(chunk);
    }
    deepStrictEqual(chunks, []);
  },
};

export const readableStreamFromCancelRejectsWhenReturnRejects = {
  async test() {
    const rejectError = new Error('return error');
    const iterable = {
      async next() {
        return { value: undefined, done: true };
      },
      async return() {
        throw rejectError;
      },
      [Symbol.asyncIterator]() {
        return this;
      },
    };

    const rs = ReadableStream.from(iterable);
    const reader = rs.getReader();

    await rejects(reader.cancel(), rejectError);
  },
};

export const readableStreamFromCancelRejectsWhenReturnThrows = {
  async test() {
    const throwError = new Error('return throws');
    const iterable = {
      async next() {
        return { value: undefined, done: true };
      },
      return() {
        throw throwError;
      },
      [Symbol.asyncIterator]() {
        return this;
      },
    };

    const rs = ReadableStream.from(iterable);
    const reader = rs.getReader();

    await rejects(reader.cancel(), (err) => err === throwError);
  },
};

export const readableStreamFromCancelRejectsWhenReturnNotMethod = {
  async test() {
    const iterable = {
      async next() {
        return { value: undefined, done: true };
      },
      return: 42, // exists but not callable
      [Symbol.asyncIterator]() {
        return this;
      },
    };

    const rs = ReadableStream.from(iterable);
    const reader = rs.getReader();

    await rejects(reader.cancel(), {
      name: 'TypeError',
      message: /return/,
    });
  },
};

export const readableStreamFromCancelRejectsWhenReturnNonObject = {
  async test() {
    const iterable = {
      async next() {
        return { value: undefined, done: true };
      },
      async return() {
        return 42; // fulfills with non-object
      },
      [Symbol.asyncIterator]() {
        return this;
      },
    };

    const rs = ReadableStream.from(iterable);
    const reader = rs.getReader();

    await rejects(reader.cancel(), {
      name: 'TypeError',
    });
  },
};

export const readableStreamFromCancelResolvesWhenReturnMissing = {
  async test() {
    const iterable = {
      async next() {
        return { value: undefined, done: true };
      },
      // no return method
      [Symbol.asyncIterator]() {
        return this;
      },
    };

    const rs = ReadableStream.from(iterable);
    const reader = rs.getReader();

    // Should resolve without error when return() is missing
    await Promise.all([reader.cancel(), reader.closed]);
  },
};

// DIVERGENCE, three ways from the spec (which REJECTS strings as
// iterables — the WPT from.any expectation): C++ iterates the string,
// yielding one chunk per code unit; TypeScript adopts it as a single
// chunk.
export const fromString = {
  async test() {
    const rs = ReadableStream.from('hi');
    const chunks = await drainToArray(rs);
    if (usingTsImpl) {
      deepStrictEqual(chunks, ['hi']);
    } else {
      deepStrictEqual(chunks, ['h', 'i']);
    }
  },
};

// The exact cancel-rejection messages for broken return() differ.
export const fromReturnValidationMessages = {
  async test() {
    {
      const rs = ReadableStream.from({
        async next() {
          return { value: 1, done: false };
        },
        return: 42,
        [Symbol.asyncIterator]() {
          return this;
        },
      });
      await rejects(rs.getReader().cancel('why'), {
        name: 'TypeError',
        message: usingTsImpl
          ? 'Iterator return() is not a function'
          : "Property 'return' is not a function",
      });
    }
    {
      const rs = ReadableStream.from({
        async next() {
          return { value: 1, done: false };
        },
        async return() {
          return 42;
        },
        [Symbol.asyncIterator]() {
          return this;
        },
      });
      await rejects(rs.getReader().cancel('why'), {
        name: 'TypeError',
        message: usingTsImpl
          ? 'The return method must return an object'
          : /Incorrect type for Promise/,
      });
    }
  },
};

// The iterator's `next` is read once, when from() opens the iterator, and
// called without arguments on every pull.
export const fromReadsNextOnce = {
  async test() {
    for (const kind of ['async', 'sync']) {
      let nextReads = 0;
      const argCounts = [];
      let i = 0;
      const iterator = {
        get next() {
          nextReads++;
          return function (...args) {
            argCounts.push(args.length);
            const result = i < 3 ? { value: i++, done: false } : { done: true };
            return kind === 'async' ? Promise.resolve(result) : result;
          };
        },
      };
      const key = kind === 'async' ? Symbol.asyncIterator : Symbol.iterator;
      const rs = ReadableStream.from({
        [key]() {
          return iterator;
        },
      });
      strictEqual(nextReads, 1, kind);
      deepStrictEqual(await drainToArray(rs), [0, 1, 2], kind);
      strictEqual(nextReads, 1, kind);
      deepStrictEqual(argCounts, [0, 0, 0, 0], kind);
    }
  },
};

// A sync iterator's result has `done` read before `value`, and `value` is
// read (and resolved) even for the final result.
export const fromSyncResultReadsDoneBeforeValue = {
  async test() {
    const log = [];
    let i = 0;
    const result = (value, done) => ({
      get done() {
        log.push('done');
        return done;
      },
      get value() {
        log.push('value');
        return value;
      },
    });
    const rs = ReadableStream.from({
      [Symbol.iterator]() {
        return {
          next() {
            return i < 2 ? result(i++, false) : result(undefined, true);
          },
        };
      },
    });
    deepStrictEqual(await drainToArray(rs), [0, 1]);
    deepStrictEqual(log, ['done', 'value', 'done', 'value', 'done', 'value']);
  },
};

// Looking up the iterator methods is a Get: a Proxy sees no `has` traps.
export const fromIteratorLookupsAreGets = {
  async test() {
    for (const kind of ['async', 'sync']) {
      const traps = [];
      const target =
        kind === 'async'
          ? {
              async *[Symbol.asyncIterator]() {
                yield 1;
              },
            }
          : {
              *[Symbol.iterator]() {
                yield 1;
              },
            };
      const proxy = new Proxy(target, {
        has(t, key) {
          traps.push(`has ${String(key)}`);
          return Reflect.has(t, key);
        },
        get(t, key, receiver) {
          traps.push(`get ${String(key)}`);
          return Reflect.get(t, key, receiver);
        },
      });
      deepStrictEqual(await drainToArray(ReadableStream.from(proxy)), [1]);
      deepStrictEqual(
        traps,
        kind === 'async'
          ? ['get Symbol(Symbol.asyncIterator)']
          : ['get Symbol(Symbol.asyncIterator)', 'get Symbol(Symbol.iterator)'],
        kind
      );
    }
  },
};

// Any object with an iterator method is accepted, functions included; a
// primitive other than a string is not, whatever its prototype carries.
export const fromAcceptsObjectsOnly = {
  async test() {
    const asyncFn = function () {};
    asyncFn[Symbol.asyncIterator] = async function* () {
      yield 'f';
    };
    deepStrictEqual(await drainToArray(ReadableStream.from(asyncFn)), ['f']);
    const syncFn = function () {};
    syncFn[Symbol.iterator] = function* () {
      yield 'g';
    };
    deepStrictEqual(await drainToArray(ReadableStream.from(syncFn)), ['g']);
    deepStrictEqual(await drainToArray(ReadableStream.from(new String('hi'))), [
      'h',
      'i',
    ]);

    for (const value of [undefined, null, true, 5, 5n, Symbol('s')]) {
      throws(() => ReadableStream.from(value), TypeError, String(value));
    }
    Object.defineProperty(Number.prototype, Symbol.iterator, {
      value: function* () {
        yield 1;
      },
      configurable: true,
      writable: true,
    });
    try {
      throws(() => ReadableStream.from(7), TypeError);
    } finally {
      Reflect.deleteProperty(Number.prototype, Symbol.iterator);
    }
    for (const value of [
      {},
      { [Symbol.iterator]: null },
      { [Symbol.asyncIterator]: 1 },
      { [Symbol.iterator]: 1 },
      { [Symbol.asyncIterator]: 1, [Symbol.iterator]: [][Symbol.iterator] },
      {
        [Symbol.iterator]() {
          return 1;
        },
      },
    ]) {
      throws(() => ReadableStream.from(value), TypeError);
    }
    // A null @@asyncIterator falls through to @@iterator.
    deepStrictEqual(
      await drainToArray(
        ReadableStream.from({
          [Symbol.asyncIterator]: null,
          *[Symbol.iterator]() {
            yield 2;
          },
        })
      ),
      [2]
    );
  },
};

// return() on cancel: read once per cancel and called with the reason; a
// null return() is absent; a throwing getter rejects the cancel.
export const fromCancelReturnLookup = {
  async test() {
    for (const kind of ['async', 'sync']) {
      const key = kind === 'async' ? Symbol.asyncIterator : Symbol.iterator;
      const from = (iterator) =>
        ReadableStream.from({
          [key]() {
            return iterator;
          },
        });
      const next = () => ({ value: 1, done: false });

      let returnReads = 0;
      let returnArgs;
      await from({
        next,
        get return() {
          returnReads++;
          return function (...args) {
            returnArgs = args;
            return {};
          };
        },
      }).cancel('why');
      strictEqual(returnReads, 1, kind);
      deepStrictEqual(returnArgs, ['why'], kind);

      await from({ next, return: null }).cancel('why');

      // C++ lets a throwing return getter escape as an uncaught exception
      // (ledger #27).
      if (!usingTsImpl) continue;
      const getterError = new Error('getter');
      strictEqual(
        await rejectionOf(
          from({
            next,
            get return() {
              throw getterError;
            },
          }).cancel('why')
        ),
        getterError,
        kind
      );
    }
  },
};

// DIVERGENCE (ledger #26): an ArrayBufferView is one chunk in TS, as a
// string is (#12). C++ iterates a typed array element by element and
// rejects a DataView (the spec does both).
export const fromArrayBufferViewIsOneChunk = {
  async test() {
    const bytes = new Uint8Array([1, 2, 3]);
    const chunks = await drainToArray(ReadableStream.from(bytes));
    if (usingTsImpl) {
      strictEqual(chunks.length, 1);
      strictEqual(chunks[0], bytes);
      const view = new DataView(new ArrayBuffer(2));
      const viewChunks = await drainToArray(ReadableStream.from(view));
      strictEqual(viewChunks.length, 1);
      strictEqual(viewChunks[0], view);
    } else {
      deepStrictEqual(chunks, [1, 2, 3]);
      throws(
        () => ReadableStream.from(new DataView(new ArrayBuffer(2))),
        TypeError
      );
    }
    throws(() => ReadableStream.from(new ArrayBuffer(2)), TypeError);
  },
};

// DIVERGENCE (ledger #27): the async-from-sync iterator's steps. TS follows
// the spec: an async iterator's final result has only `done` read; a sync
// iterator whose value rejects while not done is closed through return();
// the value of a sync iterator's final result, and of its return() result,
// is resolved, so a rejection rejects the read or the cancel; a `next` that
// is not callable rejects the read. C++ reads `value` of the final result
// too, leaves the sync iterator open, does not resolve the return() result's
// value, and ends the stream on a non-callable `next`.
export const fromIteratorProtocolEdges = {
  async test() {
    {
      const log = [];
      let i = 0;
      const result = (value, done) => ({
        get done() {
          log.push('done');
          return done;
        },
        get value() {
          log.push('value');
          return value;
        },
      });
      const rs = ReadableStream.from({
        [Symbol.asyncIterator]() {
          return {
            next() {
              return i < 1 ? result(i++, false) : result(undefined, true);
            },
          };
        },
      });
      deepStrictEqual(await drainToArray(rs), [0]);
      deepStrictEqual(
        log,
        usingTsImpl
          ? ['done', 'value', 'done']
          : ['done', 'value', 'done', 'value']
      );
    }
    {
      const valueError = new Error('value');
      let returnCalls = 0;
      const rs = ReadableStream.from({
        [Symbol.iterator]() {
          return {
            next() {
              return { value: Promise.reject(valueError), done: false };
            },
            return() {
              returnCalls++;
              throw new Error('ignored');
            },
          };
        },
      });
      strictEqual(await rejectionOf(rs.getReader().read()), valueError);
      strictEqual(returnCalls, usingTsImpl ? 1 : 0);
    }
    {
      const doneError = new Error('done value');
      let returnCalls = 0;
      const rs = ReadableStream.from({
        [Symbol.iterator]() {
          return {
            next() {
              return { value: Promise.reject(doneError), done: true };
            },
            return() {
              returnCalls++;
              return {};
            },
          };
        },
      });
      strictEqual(await rejectionOf(rs.getReader().read()), doneError);
      strictEqual(returnCalls, 0);
    }
    {
      const log = [];
      const returnError = new Error('return value');
      const rs = ReadableStream.from({
        [Symbol.iterator]() {
          return {
            next() {
              return { value: 1, done: false };
            },
            return() {
              return {
                get done() {
                  log.push('done');
                  return false;
                },
                get value() {
                  log.push('value');
                  return Promise.reject(returnError);
                },
              };
            },
          };
        },
      });
      const cancelled = rs.cancel('why');
      if (usingTsImpl) {
        strictEqual(await rejectionOf(cancelled), returnError);
      } else {
        await cancelled;
      }
      deepStrictEqual(log, ['done', 'value']);
    }
    for (const key of [Symbol.asyncIterator, Symbol.iterator]) {
      const rs = ReadableStream.from({
        [key]() {
          return { next: 5 };
        },
      });
      const reader = rs.getReader();
      if (usingTsImpl) {
        await rejects(reader.read(), TypeError);
      } else {
        strictEqual((await reader.read()).done, true);
      }
    }
  },
};
