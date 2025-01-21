// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Adapted from Node.js. Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

import timers from 'node:timers';
import timersPromises from 'node:timers/promises';
import { throws, strictEqual, deepStrictEqual, rejects, ok } from 'node:assert';

export const testEnroll = {
  async test() {
    throws(() => timers.enroll(), /Not implemented/);
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/b3641fe85d55525127c03be730596154705b798e/test/parallel/test-timers-immediate.js
export const testSetImmediate = {
  async test() {
    {
      const { promise, resolve, reject } = Promise.withResolvers();
      let mainFinished = false;
      timers.setImmediate(() => {
        strictEqual(mainFinished, true);
        timers.clearImmediate(immediateB);
        resolve();
      });

      const immediateB = timers.setImmediate(reject);
      mainFinished = true;

      await promise;
    }

    {
      const { promise, resolve } = Promise.withResolvers();
      timers.setImmediate(
        (...args) => {
          deepStrictEqual(args, [1, 2, 3]);
          resolve();
        },
        1,
        2,
        3
      );
      await promise;
    }
  },
};

export const testSetTimeout = {
  async test() {
    {
      const { promise, resolve, reject } = Promise.withResolvers();
      let mainFinished = false;
      timers.setTimeout(() => {
        strictEqual(mainFinished, true);
        timers.clearTimeout(timeoutB);
        resolve();
      });

      const timeoutB = timers.setTimeout(reject);
      mainFinished = true;

      await promise;
    }

    {
      const { promise, resolve } = Promise.withResolvers();
      timers.setTimeout(
        (...args) => {
          deepStrictEqual(args, [1, 2, 3]);
          resolve();
        },
        100,
        1,
        2,
        3
      );
      await promise;
    }
  },
};

export const testSetInterval = {
  async test() {
    {
      const { promise, resolve, reject } = Promise.withResolvers();
      let mainFinished = false;
      const thisInterval = timers.setInterval(() => {
        strictEqual(mainFinished, true);
        timers.clearInterval(intervalB);
        timers.clearInterval(thisInterval);
        resolve();
      }, 100);

      const intervalB = timers.setInterval(reject, 100);
      mainFinished = true;

      await promise;
    }

    {
      const { promise, resolve } = Promise.withResolvers();
      const thisInterval = timers.setInterval(
        (...args) => {
          deepStrictEqual(args, [1, 2, 3]);
          timers.clearInterval(thisInterval);
          resolve();
        },
        100,
        1,
        2,
        3
      );
      await promise;
    }
  },
};

export const testRefresh = {
  async test() {
    const { promise, resolve, reject } = Promise.withResolvers();
    timers.clearTimeout(
      timers
        .setTimeout(() => {
          reject();
        }, 1)
        .refresh()
    );

    timers.setTimeout(() => {
      resolve();
    }, 2);

    await promise;
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/7bc2946293757389468f1fa09714860f8e1147b7/test/parallel/test-timers-immediate-promisified.js
export const timersImmediatePromise = {
  async test() {
    {
      const ac = new AbortController();
      const signal = ac.signal;
      const { promise, resolve } = Promise.withResolvers();
      rejects(timersPromises.setImmediate(10, { signal }), /AbortError/).then(
        resolve
      );
      ac.abort();
      await promise;
    }

    {
      const signal = AbortSignal.abort(); // Abort in advance
      await rejects(timersPromises.setImmediate(10, { signal }), /AbortError/);
    }

    {
      // Check that aborting after resolve will not reject.
      const ac = new AbortController();
      const signal = ac.signal;
      await timersPromises.setImmediate(10, { signal }).then(() => {
        ac.abort();
      });
    }

    {
      await Promise.all(
        [1, '', false, Infinity].map((i) =>
          rejects(timersPromises.setImmediate(10, i), {
            code: 'ERR_INVALID_ARG_TYPE',
          })
        )
      );

      await Promise.all(
        [1, '', false, Infinity, null, {}].map((signal) =>
          rejects(timersPromises.setImmediate(10, { signal }), {
            code: 'ERR_INVALID_ARG_TYPE',
          })
        )
      );

      await Promise.all(
        [1, '', Infinity, null, {}].map((ref) =>
          rejects(timersPromises.setImmediate(10, { ref }), {
            code: 'ERR_INVALID_ARG_TYPE',
          })
        )
      );
    }

    {
      const signal = AbortSignal.abort('boom');
      await rejects(timersPromises.setImmediate(undefined, { signal }), {
        cause: 'boom',
      });
    }
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/7bc2946293757389468f1fa09714860f8e1147b7/test/parallel/test-timers-timeout-promisified.js
export const timersTimeoutPromise = {
  async test() {
    {
      const promise = timersPromises.setTimeout(1);
      strictEqual(await promise, undefined);
    }

    {
      const promise = timersPromises.setTimeout(1, 'foobar');
      strictEqual(await promise, 'foobar');
    }

    {
      const ac = new AbortController();
      const signal = ac.signal;
      const { promise, resolve } = Promise.withResolvers();
      rejects(
        timersPromises.setTimeout(10, undefined, { signal }),
        /AbortError/
      ).then(resolve);
      ac.abort();
      await promise;
    }

    {
      const signal = AbortSignal.abort(); // Abort in advance
      await rejects(
        timersPromises.setTimeout(10, undefined, { signal }),
        /AbortError/
      );
    }

    {
      // Check that aborting after resolve will not reject.
      const ac = new AbortController();
      const signal = ac.signal;
      await timersPromises.setTimeout(10, undefined, { signal }).then(() => {
        ac.abort();
      });
    }

    {
      for (const delay of ['', false]) {
        await rejects(
          () => timersPromises.setTimeout(delay, null, {}),
          /ERR_INVALID_ARG_TYPE/
        );
      }

      for (const options of [1, '', false, Infinity]) {
        await rejects(
          () => timersPromises.setTimeout(10, null, options),
          /ERR_INVALID_ARG_TYPE/
        );
      }

      for (const signal of [1, '', false, Infinity, null, {}]) {
        await rejects(
          () => timersPromises.setTimeout(10, null, { signal }),
          /ERR_INVALID_ARG_TYPE/
        );
      }

      for (const ref of [1, '', Infinity, null, {}]) {
        await rejects(
          () => timersPromises.setTimeout(10, null, { ref }),
          /ERR_INVALID_ARG_TYPE/
        );
      }
    }

    {
      const signal = AbortSignal.abort('boom');
      await rejects(timersPromises.setTimeout(1, undefined, { signal }), {
        cause: 'boom',
      });
    }
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/7bc2946293757389468f1fa09714860f8e1147b7/test/parallel/test-timers-interval-promisified.js
export const timersIntervalPromise = {
  async test() {
    {
      const iterable = timersPromises.setInterval(1, undefined);
      const iterator = iterable[Symbol.asyncIterator]();
      const result = iterator.next();
      ok(!result.done, 'iterator was wrongly marked as done');
      strictEqual(result.value, undefined);
      await iterator.return();
    }

    {
      const iterable = timersPromises.setInterval(1, 'foobar');
      const iterator = iterable[Symbol.asyncIterator]();
      const result = await iterator.next();
      ok(!result.done, 'iterator was wronly marked as done');
      strictEqual(result.value, 'foobar');
      await iterator.return();
    }

    {
      const iterable = timersPromises.setInterval(1, 'foobar');
      const iterator = iterable[Symbol.asyncIterator]();
      const result = await iterator.next();

      ok(!result.done, 'iterator was wronly marked as done');
      strictEqual(result.value, 'foobar');
      await iterator.next();
    }

    {
      const signal = AbortSignal.abort(); // Abort in advance

      const iterable = timersPromises.setInterval(1, undefined, { signal });
      const iterator = iterable[Symbol.asyncIterator]();
      await rejects(iterator.next(), /AbortError/);
    }

    {
      const ac = new AbortController();
      const { signal } = ac;

      const iterable = timersPromises.setInterval(100, undefined, { signal });
      const iterator = iterable[Symbol.asyncIterator]();

      // This promise should take 100 seconds to resolve, so now aborting it should
      // mean we abort early
      const promise = iterator.next();

      ac.abort(); // Abort in after we have a next promise

      await rejects(promise, /AbortError/);
    }

    {
      // Check aborting after getting a value.
      const ac = new AbortController();
      const { signal } = ac;

      const iterable = timersPromises.setInterval(100, undefined, { signal });
      const iterator = iterable[Symbol.asyncIterator]();

      const promise = iterator.next();
      const abortPromise = promise.then(ac.abort()).then(() => iterator.next());
      await rejects(abortPromise, /AbortError/);
    }

    {
      for (const ref of [1, '', Infinity, null, {}]) {
        const iterable = timersPromises.setInterval(10, undefined, { ref });
        await rejects(
          () => iterable[Symbol.asyncIterator]().next(),
          /ERR_INVALID_ARG_TYPE/
        );
      }

      for (const signal of [1, '', Infinity, null, {}]) {
        const iterable = timersPromises.setInterval(10, undefined, { signal });
        await rejects(
          () => iterable[Symbol.asyncIterator]().next(),
          /ERR_INVALID_ARG_TYPE/
        );
      }

      for (const options of [1, '', Infinity, null, true, false]) {
        const iterable = timersPromises.setInterval(10, undefined, options);
        await rejects(
          () => iterable[Symbol.asyncIterator]().next(),
          /ERR_INVALID_ARG_TYPE/
        );
      }
    }

    {
      async function runInterval(fn, intervalTime, signal) {
        const input = 'foobar';
        const interval = timersPromises.setInterval(intervalTime, input, {
          signal,
        });
        let iteration = 0;
        for await (const value of interval) {
          strictEqual(value, input);
          iteration++;
          await fn(iteration);
        }
      }

      {
        // Check that we call the correct amount of times.
        const controller = new AbortController();
        const { signal } = controller;

        let loopCount = 0;
        const delay = 20;
        const timeoutLoop = runInterval(
          () => {
            loopCount++;
            if (loopCount === 5) controller.abort();
            if (loopCount > 5) throw new Error('ran too many times');
          },
          delay,
          signal
        );

        await rejects(timeoutLoop, /AbortError/);
        strictEqual(loopCount, 5);
      }

      {
        // Check that if we abort when we have some unresolved callbacks,
        // we actually call them.
        const controller = new AbortController();
        const { signal } = controller;
        const delay = 10;
        let totalIterations = 0;
        const timeoutLoop = runInterval(
          async (iterationNumber) => {
            await timersPromises.setTimeout(delay * 4);
            if (iterationNumber <= 2) {
              strictEqual(signal.aborted, false);
            }
            if (iterationNumber === 2) {
              controller.abort();
            }
            if (iterationNumber > 2) {
              strictEqual(signal.aborted, true);
            }
            if (iterationNumber > totalIterations) {
              totalIterations = iterationNumber;
            }
          },
          delay,
          signal
        );

        await timeoutLoop.catch(() => {
          ok(totalIterations >= 3, `iterations was ${totalIterations} < 3`);
        });
      }
    }

    {
      // Check that the timing is correct
      let pre = false;
      let post = false;

      const time_unit = 50;
      await Promise.all([
        timersPromises.setTimeout(1).then(() => (pre = true)),
        new Promise((res) => {
          const iterable = timersPromises.setInterval(time_unit * 2);
          const iterator = iterable[Symbol.asyncIterator]();

          iterator
            .next()
            .then(() => {
              ok(pre, 'interval ran too early');
              ok(!post, 'interval ran too late');
              return iterator.next();
            })
            .then(() => {
              ok(post, 'second interval ran too early');
              return iterator.return();
            })
            .then(res);
        }),
        timersPromises.setTimeout(time_unit * 3).then(() => (post = true)),
      ]);
    }
  },
};
