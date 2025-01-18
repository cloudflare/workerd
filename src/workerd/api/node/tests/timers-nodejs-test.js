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
import { throws, strictEqual, deepStrictEqual } from 'node:assert';

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
      globalThis.setImmediate(
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
