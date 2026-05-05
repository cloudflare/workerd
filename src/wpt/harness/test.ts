// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright © web-platform-tests contributors. BSD license
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

import {
  FilterList,
  type UnknownFunc,
  type TestFn,
  type PromiseTestFn,
} from './common';

declare global {
  function promise_test(
    func: PromiseTestFn,
    name?: string,
    properties?: unknown
  ): void;
  function async_test(
    func: TestFn | string,
    name?: string,
    properties?: unknown
  ): Test;
  function test(func: TestFn, name?: string, properties?: unknown): void;
}

type TestErrorType = Error | 'OMITTED' | 'DISABLED' | undefined;

/**
 * A single subtest. A Test is not constructed directly but via the
 * :js:func:`test`, :js:func:`async_test` or :js:func:`promise_test` functions.
 *
 * @param name - This must be unique in a given file and must be
 * invariant between runs.
 *
 */
/* eslint-disable @typescript-eslint/no-this-alias -- WPT allows for overriding the this environment for a step but defaults to the Test class */
export class Test {
  static Phases = {
    INITIAL: 0,
    STARTED: 1,
    HAS_RESULT: 2,
    CLEANING: 3,
    COMPLETE: 4,
  } as const;

  name: string;
  properties: unknown;
  phase: (typeof Test.Phases)[keyof typeof Test.Phases];
  cleanup_callbacks: UnknownFunc[] = [];

  error: TestErrorType = undefined;

  // If this test is asynchronous, stores a promise that resolves on test completion
  promise?: Promise<void>;

  constructor(name: string, properties?: unknown) {
    this.name = name;
    this.properties = properties;
    this.phase = Test.Phases.INITIAL;
  }

  /**
   * Run a single step of an ongoing test.
   *
   * @param func - Callback function to run as a step. If
   * this throws an :js:func:`AssertionError`, or any other
   * exception, the :js:class:`Test` status is set to ``FAIL``.
   * @param [this_obj] - The object to use as the this
   * value when calling ``func``. Defaults to the  :js:class:`Test` object.
   */
  step(func: UnknownFunc, this_obj?: object, ...rest: unknown[]): unknown {
    if (this.phase > Test.Phases.STARTED) {
      return undefined;
    }

    if (arguments.length === 1) {
      this_obj = this;
    }

    try {
      return func.call(this_obj, ...rest);
    } catch (err) {
      if (this.phase >= Test.Phases.HAS_RESULT) {
        return undefined;
      }

      this.error = new AggregateError([err], this.name);
      this.error.stack = '';
      this.done();
    }

    return undefined;
  }

  /**
   * Wrap a function so that it runs as a step of the current test.
   *
   * This allows creating a callback function that will run as a
   * test step.
   *
   * @example
   * let t = async_test("Example");
   * onload = t.step_func(e => {
   *   assert_equals(e.name, "load");
   *   // Mark the test as complete.
   *   t.done();
   * })
   *
   * @param func - Function to run as a step. If this
   * throws an :js:func:`AssertionError`, or any other exception,
   * the :js:class:`Test` status is set to ``FAIL``.
   * @param [this_obj] - The object to use as the this
   * value when calling ``func``. Defaults to the :js:class:`Test` object.
   */
  step_func(func: UnknownFunc, this_obj?: object): UnknownFunc {
    if (arguments.length === 1) {
      this_obj = this;
    }

    return (...params: unknown[]) => this.step(func, this_obj, ...params);
  }

  /**
   * Wrap a function so that it runs as a step of the current test,
   * and automatically marks the test as complete if the function
   * returns without error.
   *
   * @param func - Function to run as a step. If this
   * throws an :js:func:`AssertionError`, or any other exception,
   * the :js:class:`Test` status is set to ``FAIL``. If it returns
   * without error the status is set to ``PASS``.
   * @param [this_obj] - The object to use as the this
   * value when calling `func`. Defaults to the :js:class:`Test` object.
   */
  step_func_done(func?: UnknownFunc, this_obj?: object): UnknownFunc {
    if (arguments.length === 1) {
      this_obj = this;
    }

    return (...params: unknown[]) => {
      if (func) {
        this.step(func, this_obj, ...params);
      }

      this.done();
    };
  }

  /**
   * Return a function that automatically sets the current test to
   * ``FAIL`` if it's called.
   *
   * @param [description] - Error message to add to assert
   * in case of failure.
   *
   */
  unreached_func(description?: string): UnknownFunc {
    return this.step_func(() => {
      assert_unreached(description);
    });
  }

  /**
   * Run a function as a step of the test after a given timeout.
   *
   * In general it's encouraged to use :js:func:`Test.step_wait` or
   * :js:func:`step_wait_func` in preference to this function where possible,
   * as they provide better test performance.
   *
   * @param func - Function to run as a test
   * step.
   * @param timeout - Time in ms to wait before running the
   * test step.
   *
   */
  step_timeout(
    func: UnknownFunc,
    timeout: number,
    ...rest: unknown[]
  ): ReturnType<typeof setTimeout> {
    return setTimeout(
      this.step_func(() => func(...rest)),
      timeout
    );
  }

  add_cleanup(func: UnknownFunc): void {
    this.cleanup_callbacks.push(func);
  }

  done(): void {
    if (this.phase >= Test.Phases.CLEANING) {
      return;
    }

    this.cleanup();
  }

  cleanup(): void {
    // TODO(soon): Cleanup functions can also return a promise instead of being synchronous, but we don't need this for any tests currently.
    for (const cleanFn of this.cleanup_callbacks) {
      cleanFn();
    }
    this.phase = Test.Phases.COMPLETE;
  }
}

/* eslint-enable @typescript-eslint/no-this-alias */
class SkippedTest extends Test {
  constructor(name: string, reason: TestErrorType) {
    super(name);
    this.error = reason;
  }

  override step(
    _func: UnknownFunc,
    _this_obj?: object,
    ..._rest: unknown[]
  ): unknown {
    return undefined;
  }
}

class PromiseTest extends Test {
  // TODO(soon): Extract out promise_test specific behaviour to make code easier to understand.
}

globalThis.promise_test = (func, name, properties): void => {
  if (maybeAddSkippedTest(name ?? '')) {
    return;
  }

  const testCase = new PromiseTest(name ?? '', properties);
  globalThis.state.subtests.push(testCase);

  const promise = testCase.step(func, testCase, testCase);

  if (!(promise instanceof Promise)) {
    // The functions passed to promise_test are expected to return a Promise,
    // but are not required to be async functions. That means they could throw
    // an error immediately when run.

    if (!testCase.error) {
      testCase.error = new Error('Unexpected value returned from promise_test');
    }

    return;
  }

  testCase.promise = promise
    .then(() => {
      testCase.done();
    })
    .catch((err: unknown) => {
      testCase.error = Object.assign(new AggregateError([err], name), {
        stack: '',
      });
    });
};

class AsyncTest extends Test {
  #resolve: () => void;

  constructor(name: string, properties: unknown) {
    super(name, properties);

    // eslint-disable-next-line @typescript-eslint/no-invalid-void-type -- void is being used as a valid generic in this context
    const { promise, resolve } = Promise.withResolvers<void>();
    this.promise = promise;
    this.#resolve = resolve;
  }

  override done(): void {
    super.done();
    this.#resolve();
  }
}

globalThis.async_test = (func, name, properties): Test => {
  // async_test can be called in two ways:
  // 1. async_test(func, name, properties) - func is a TestFn
  // 2. async_test(name, properties) - just creates a test with the given name
  let testName: string;
  let testFunc: TestFn | undefined;

  if (typeof func === 'string') {
    // async_test(name, properties) signature
    testName = func;
    testFunc = undefined;
    // name parameter is actually properties in this case
    properties = name;
  } else {
    // async_test(func, name, properties) signature
    testName = name ?? '';
    testFunc = func;
  }

  if (maybeAddSkippedTest(testName)) {
    // Return a dummy test object for skipped tests
    return new SkippedTest(testName, 'DISABLED');
  }

  const testCase = new AsyncTest(testName, properties);
  globalThis.state.subtests.push(testCase);

  if (testFunc) {
    testCase.step(testFunc, testCase, testCase);
  }

  return testCase;
};

/**
 * Create a synchronous test
 *
 * @param func - Test function. This is executed
 * immediately. If it returns without error, the test status is
 * set to ``PASS``. If it throws an :js:class:`AssertionError`, or
 * any other exception, the test status is set to ``FAIL``
 * (typically from an `assert` function).
 * @param name - Test name. This must be unique in a
 * given file and must be invariant between runs.
 */
globalThis.test = (func, name, properties): void => {
  if (maybeAddSkippedTest(name ?? '')) {
    return;
  }

  const testCase = new Test(name ?? '', properties);
  globalThis.state.subtests.push(testCase);

  testCase.step(func, testCase, testCase);
  testCase.done();
};

function maybeAddSkippedTest(message: string): boolean {
  const disabledTests = new FilterList(globalThis.state.options.disabledTests);

  if (disabledTests.has(message)) {
    globalThis.state.subtests.push(new SkippedTest(message, 'DISABLED'));
    return true;
  }

  const omittedTests = new FilterList(globalThis.state.options.omittedTests);

  if (omittedTests.has(message)) {
    globalThis.state.subtests.push(new SkippedTest(message, 'OMITTED'));
    return true;
  }

  if (globalThis.state.options.verbose) {
    console.info('run', message);
  }

  return false;
}
