// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// ReadableStream.from(): iterable/async-iterable adoption and cancel
// plumbing through the iterator protocol. Migrated from streams-test.js;
// the string divergence and the exact rejection messages are pinned at
// the end.

import { strictEqual, deepStrictEqual, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { drainToArray } from 'helpers';

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
