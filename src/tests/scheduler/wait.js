// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// scheduler.wait(): its receiver, the conversion of its arguments, what its
// promise settles with, and when.

import { deepStrictEqual, ok, strictEqual } from 'node:assert';
import { caught, framesOf, isBootstrapFrame } from 'helpers';
import { capturesThrows, usingTsImpl } from 'which-impl';

const kPrototype = Object.getPrototypeOf(scheduler);
const wait = kPrototype.wait;

const ILLEGAL_INVOCATION =
  'Illegal invocation: function called with incorrect `this` reference. See ' +
  'https://developers.cloudflare.com/workers/observability/errors/' +
  '#illegal-invocation-errors for details.';
const NO_DELAY =
  "Failed to execute 'wait' on 'Scheduler': " +
  "parameter 1 is not of type 'number'.";
const BAD_OPTIONS =
  "Failed to execute 'wait' on 'Scheduler': " +
  "parameter 2 is not of type 'WaitOptions'.";
const BAD_SIGNAL =
  "Incorrect type for the 'signal' field on 'WaitOptions': the provided " +
  "value is not of type 'AbortSignal'.";
const OUTSIDE_OF_A_REQUEST =
  'Disallowed operation called within global scope. Asynchronous I/O (ex: ' +
  'fetch() or connect()), setting a timeout, and generating random values ' +
  'are not allowed within global scope. To fix this error, perform this ' +
  'operation within a handler. ' +
  'https://developers.cloudflare.com/workers/runtime-apis/handlers/';

// What a call of wait() does: 'threw' with what it threw, or, once the
// promise it returned settles, 'resolved' with the value or 'rejected' with
// the reason.
async function outcome(call) {
  let promise;
  try {
    promise = call();
  } catch (error) {
    return { how: 'threw', value: error };
  }
  strictEqual(Object.getPrototypeOf(promise), Promise.prototype);
  try {
    return { how: 'resolved', value: await promise };
  } catch (error) {
    return { how: 'rejected', value: error };
  }
}

async function resolvesWith(call) {
  const { how, value } = await outcome(call);
  strictEqual(how, 'resolved', String(value));
  strictEqual(value, undefined);
}

// Asserts the call returns a promise that rejects with an error of this
// realm's `type` and exactly `message`, and returns the error.
async function rejectsWith(call, type, message, how = 'rejected') {
  const result = await outcome(call);
  strictEqual(result.how, how, String(result.value));
  strictEqual(Object.getPrototypeOf(result.value), type.prototype);
  strictEqual(result.value.message, message);
  return result.value;
}

// How wait() reports a failure past its receiver check, other than an abort:
// it rejects under capture_async_api_throws, and throws without.
const kFails = capturesThrows ? 'rejected' : 'threw';

function failsWith(call, type, message) {
  return rejectsWith(call, type, message, kFails);
}

// Calls made as this module loads, outside of a request.
const topLevel = outcome(() => scheduler.wait(1));
const topLevelAborted = outcome(() =>
  scheduler.wait(1, { signal: AbortSignal.abort(new RangeError('early')) })
);
const topLevelBadDelay = outcome(() => scheduler.wait());

export const receiver = {
  async test() {
    // The receiver is checked first and its error is thrown, not a
    // rejection, even when an argument is wrong too.
    for (const value of [
      undefined,
      null,
      {},
      kPrototype,
      Object.create(scheduler),
      Object.create(kPrototype),
      globalThis,
    ]) {
      for (const args of [[1], [], [1, 5]]) {
        const { how, value: error } = await outcome(() =>
          wait.apply(value, args)
        );
        strictEqual(how, 'threw');
        strictEqual(Object.getPrototypeOf(error), TypeError.prototype);
        strictEqual(error.message, ILLEGAL_INVOCATION);
      }
    }
    await resolvesWith(() => wait.call(scheduler, 1));
    const { wait: detached } = scheduler;
    strictEqual(caught(() => detached(1)).message, ILLEGAL_INVOCATION);
  },
};

export const receiverErrorStack = {
  test() {
    function calls() {
      return caught(() => wait.call({}, 1));
    }
    const error = calls();
    ok(!framesOf(error).some(isBootstrapFrame), error.stack);
    ok(error !== calls());
  },
};

export const failureStack = {
  async test() {
    async function fails(call) {
      const { value } = await outcome(call);
      ok(value instanceof TypeError, String(value));
      return framesOf(value);
    }
    // The errors of the argument count and of options that are no object
    // start at the caller, in both implementations.
    for (const call of [() => scheduler.wait(), () => scheduler.wait(1, 5)]) {
      ok(!(await fails(call)).some(isBootstrapFrame));
    }
    // Ledger: those of the delay's conversion and of a `signal` that is no
    // AbortSignal start, in the TypeScript implementation, at its wait().
    for (const call of [
      () => scheduler.wait(Symbol()),
      () => scheduler.wait(1, { signal: 5 }),
    ]) {
      const frames = await fails(call);
      strictEqual(isBootstrapFrame(frames[0]), usingTsImpl, frames[0]);
      ok(!frames.slice(1).some(isBootstrapFrame));
    }
  },
};

export const resolves = {
  async test() {
    const started = Date.now();
    await resolvesWith(() => scheduler.wait(20));
    ok(Date.now() - started >= 15);
    // A promise of this realm each time.
    const first = scheduler.wait(0);
    const second = scheduler.wait(0);
    ok(first !== second);
    await Promise.all([first, second]);
  },
};

export const delayConversion = {
  async test() {
    // No delay at all rejects; any value is converted to a number.
    await failsWith(() => scheduler.wait(), TypeError, NO_DELAY);
    for (const delay of [
      undefined,
      null,
      NaN,
      -1,
      -Infinity,
      0,
      -0,
      1.5,
      '2',
      'x',
      '',
      true,
      {},
      [],
      [3],
      () => {},
      {
        valueOf() {
          return 1;
        },
      },
      new Number(1),
    ]) {
      await resolvesWith(() => scheduler.wait(delay));
    }
    await failsWith(
      () => scheduler.wait(Symbol()),
      TypeError,
      'Cannot convert a Symbol value to a number'
    );
    await failsWith(
      () => scheduler.wait(1n),
      TypeError,
      'Cannot convert a BigInt value to a number'
    );
    // What the conversion throws is the rejection, itself.
    const thrown = new RangeError('valueOf');
    const { how, value } = await outcome(() =>
      scheduler.wait({
        valueOf() {
          throw thrown;
        },
      })
    );
    strictEqual(how, kFails);
    strictEqual(value, thrown);
    // Too long a delay stays pending.
    for (const delay of [Infinity, 2 ** 31, 1e308]) {
      const settled = await Promise.race([
        outcome(() => scheduler.wait(delay)),
        new Promise((resolve) => setTimeout(resolve, 20, 'pending')),
      ]);
      strictEqual(settled, 'pending');
    }
  },
};

export const optionsConversion = {
  async test() {
    for (const options of [
      undefined,
      null,
      {},
      [],
      () => {},
      new Map(),
      { signal: undefined },
      { other: 1 },
      Object.create({ signal: undefined }),
      // An object that inherits from a signal stands for it.
      { signal: Object.create(new AbortController().signal) },
    ]) {
      await resolvesWith(() => scheduler.wait(1, options));
    }
    for (const options of [5, 0, 's', '', true, false, 1n, Symbol()]) {
      await failsWith(() => scheduler.wait(1, options), TypeError, BAD_OPTIONS);
    }
    for (const signal of [
      null,
      {},
      5,
      's',
      false,
      new AbortController(),
      { aborted: false, reason: undefined },
      AbortSignal.prototype,
    ]) {
      await failsWith(
        () => scheduler.wait(1, { signal }),
        TypeError,
        BAD_SIGNAL
      );
    }
    // The signal is read once, also when inherited or from a function.
    let reads = 0;
    const { signal } = new AbortController();
    await resolvesWith(() =>
      scheduler.wait(
        1,
        Object.create({
          get signal() {
            reads++;
            return signal;
          },
        })
      )
    );
    strictEqual(reads, 1);
    const callable = () => {};
    callable.signal = AbortSignal.abort(new RangeError('callable'));
    await rejectsWith(
      () => scheduler.wait(1, callable),
      RangeError,
      'callable'
    );
    // What the getter throws is the rejection, itself.
    const thrown = new RangeError('getter');
    const { how, value } = await outcome(() =>
      scheduler.wait(1, {
        get signal() {
          throw thrown;
        },
      })
    );
    strictEqual(how, kFails);
    strictEqual(value, thrown);
    // Extra arguments are ignored.
    await resolvesWith(() => scheduler.wait(1, undefined, 5));
  },
};

export const conversionOrder = {
  async test() {
    const steps = [];
    await resolvesWith(() =>
      scheduler.wait(
        {
          valueOf() {
            steps.push('delay');
            return 1;
          },
        },
        {
          get signal() {
            steps.push('signal');
            return undefined;
          },
        }
      )
    );
    deepStrictEqual(steps, ['delay', 'signal']);
    // A delay that fails to convert comes before options that do.
    await failsWith(
      () => scheduler.wait(Symbol(), 5),
      TypeError,
      'Cannot convert a Symbol value to a number'
    );
    await failsWith(() => scheduler.wait(1, 5), TypeError, BAD_OPTIONS);
  },
};

// What the promise rejects with for each abort reason: an error is recreated
// (the reason crosses the runtime's event loop), and any other value becomes
// an Error whose message describes it.
const kReasons = [
  [new RangeError('range'), RangeError, 'range'],
  [new TypeError('type'), TypeError, 'type'],
  [new Error('plain'), Error, 'plain'],
  [new DOMException('dom', 'DataError'), DOMException, 'dom'],
  [undefined, DOMException, 'The operation was aborted'],
  ['text', Error, 'text'],
  [42, Error, '42'],
  [null, Error, 'null'],
];

export const alreadyAborted = {
  async test() {
    for (const [reason, type, message] of kReasons) {
      const signal = AbortSignal.abort(reason);
      const error = await rejectsWith(
        () => scheduler.wait(1000, { signal }),
        type,
        message
      );
      ok(error !== reason);
      if (reason instanceof DOMException) strictEqual(error.name, reason.name);
    }
    const error = await rejectsWith(
      () => scheduler.wait(1000, { signal: AbortSignal.abort() }),
      DOMException,
      'The operation was aborted'
    );
    strictEqual(error.name, 'AbortError');
  },
};

export const abortedWhileWaiting = {
  async test() {
    for (const [reason, type, message] of kReasons) {
      const controller = new AbortController();
      const started = Date.now();
      const settled = rejectsWith(
        () => scheduler.wait(10_000, { signal: controller.signal }),
        type,
        message
      );
      controller.abort(reason);
      const error = await settled;
      ok(Date.now() - started < 5000);
      ok(error !== reason);
    }
    // A signal that follows another, and one that times out.
    const controller = new AbortController();
    const any = AbortSignal.any([controller.signal]);
    const settled = rejectsWith(
      () => scheduler.wait(10_000, { signal: any }),
      RangeError,
      'any'
    );
    controller.abort(new RangeError('any'));
    await settled;
    const timedOut = await outcome(() =>
      scheduler.wait(10_000, { signal: AbortSignal.timeout(5) })
    );
    strictEqual(timedOut.how, 'rejected');
    strictEqual(timedOut.value.name, 'TimeoutError');
  },
};

export const abortOrdering = {
  async test() {
    // The rejection comes through the runtime's event loop, so after the
    // 'abort' event and the microtasks queued by then.
    const controller = new AbortController();
    const steps = [];
    controller.signal.addEventListener('abort', () => steps.push('event'));
    const settled = scheduler
      .wait(10_000, { signal: controller.signal })
      .catch(() => steps.push('rejected'));
    controller.abort();
    steps.push('abort() returned');
    await Promise.resolve();
    steps.push('microtask');
    await settled;
    deepStrictEqual(steps, [
      'event',
      'abort() returned',
      'microtask',
      'rejected',
    ]);
  },
};

export const abortAfterTheWait = {
  async test() {
    const controller = new AbortController();
    await resolvesWith(() => scheduler.wait(1, { signal: controller.signal }));
    controller.abort();
    // The same signal serves many waits, and the one that aborts them all.
    const shared = new AbortController();
    const waits = [];
    for (let i = 0; i < 5; i++) {
      waits.push(
        outcome(() => scheduler.wait(10_000, { signal: shared.signal }))
      );
    }
    shared.abort(new RangeError('shared'));
    for (const { how, value } of await Promise.all(waits)) {
      strictEqual(how, 'rejected');
      strictEqual(value.message, 'shared');
    }
  },
};

export const timerOrder = {
  async test() {
    // The waits are timers among setTimeout()'s.
    const steps = [];
    const done = [];
    done.push(scheduler.wait(30).then(() => steps.push('wait 30')));
    done.push(
      new Promise((resolve) =>
        setTimeout(() => {
          steps.push('timeout 15');
          resolve();
        }, 15)
      )
    );
    done.push(scheduler.wait(0).then(() => steps.push('wait 0')));
    await Promise.all(done);
    deepStrictEqual(steps, ['wait 0', 'timeout 15', 'wait 30']);
  },
};

export const replacedBuiltIns = {
  async test() {
    // wait() calls nothing user code can replace: each replacement records
    // its name and throws.
    const called = [];
    const { defineProperty, getOwnPropertyDescriptor } = Object;
    const replacements = [
      [Object.getPrototypeOf(globalThis), 'setTimeout'],
      [Object.getPrototypeOf(globalThis), 'clearTimeout'],
      [globalThis, 'Promise'],
      [globalThis, 'TypeError'],
      [Promise, 'reject'],
      [Promise, 'resolve'],
      [Promise.prototype, 'then'],
      [Promise.prototype, 'catch'],
      [Error, 'captureStackTrace'],
      [EventTarget.prototype, 'addEventListener'],
      [AbortSignal.prototype, 'throwIfAborted'],
      [Function.prototype, 'apply'],
      [Function.prototype, 'bind'],
    ];
    const controller = new AbortController();
    const saved = [];
    for (let i = 0; i < replacements.length; i++) {
      saved[i] = getOwnPropertyDescriptor(
        replacements[i][0],
        replacements[i][1]
      );
      ok(saved[i] !== undefined, replacements[i][1]);
    }
    let resolved;
    let aborted;
    let badDelay;
    let badReceiver;
    try {
      for (let i = 0; i < replacements.length; i++) {
        defineProperty(replacements[i][0], replacements[i][1], {
          __proto__: null,
          value() {
            called[called.length] = String(replacements[i][1]);
            throw new Error('a replaced built-in was called');
          },
          writable: true,
          configurable: true,
        });
      }
      resolved = scheduler.wait(1);
      aborted = scheduler.wait(10_000, { signal: controller.signal });
      try {
        badDelay = scheduler.wait();
      } catch (error) {
        badDelay = { thrown: error };
      }
      try {
        wait(1);
      } catch (error) {
        badReceiver = error;
      }
    } finally {
      for (let i = replacements.length - 1; i >= 0; i--) {
        defineProperty(replacements[i][0], replacements[i][1], saved[i]);
      }
    }
    deepStrictEqual(called, []);
    strictEqual(badReceiver.message, ILLEGAL_INVOCATION);
    strictEqual(Object.getPrototypeOf(badReceiver), TypeError.prototype);
    await resolvesWith(() => resolved);
    const failed = await outcome(() => {
      if (Object.hasOwn(badDelay, 'thrown')) throw badDelay.thrown;
      return badDelay;
    });
    strictEqual(failed.how, kFails);
    strictEqual(failed.value.message, NO_DELAY);
    strictEqual(Object.getPrototypeOf(failed.value), TypeError.prototype);
    controller.abort(new RangeError('replaced'));
    await rejectsWith(() => aborted, RangeError, 'replaced');
  },
};

export const outsideOfARequest = {
  async test() {
    // Setting a timer needs a request.
    const noTimer = await topLevel;
    strictEqual(noTimer.how, kFails);
    strictEqual(Object.getPrototypeOf(noTimer.value), Error.prototype);
    strictEqual(noTimer.value.message, OUTSIDE_OF_A_REQUEST);
    // So does the rejection of a signal that has aborted.
    const aborted = await topLevelAborted;
    strictEqual(aborted.how, kFails);
    strictEqual(aborted.value.message, OUTSIDE_OF_A_REQUEST);
    // The arguments are converted first.
    const { how, value } = await topLevelBadDelay;
    strictEqual(how, kFails);
    strictEqual(value.message, NO_DELAY);
  },
};
