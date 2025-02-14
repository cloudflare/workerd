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
  strictEqual,
  notStrictEqual,
  deepStrictEqual,
  ok,
  throws,
  fail,
  type AssertPredicate,
} from 'node:assert';

import { default as path } from 'node:path';

type CommonOptions = {
  comment?: string;
  verbose?: boolean;
};

type SuccessOptions = {
  expectedFailures?: undefined;
  skippedTests?: undefined;
  skipAllTests?: false;
};

type ErrorOptions = {
  // A comment is mandatory when there are expected failures or skipped tests
  comment: string;
  expectedFailures?: string[];
  skippedTests?: string[];
  skipAllTests?: boolean;
};

type TestRunnerOptions = CommonOptions & (SuccessOptions | ErrorOptions);

export type TestRunnerConfig = {
  [key: string]: TestRunnerOptions;
};

type Env = {
  unsafe: { eval: (code: string) => void };
  [key: string]: unknown;
};

type TestCase = {
  test(_: unknown, env: Env): Promise<void>;
};

type UnknownFunc = (...args: unknown[]) => unknown;

/**
 * A single subtest. A Test is not constructed directly but via the
 * :js:func:`test`, :js:func:`async_test` or :js:func:`promise_test` functions.
 *
 * @param name - This must be unique in a given file and must be
 * invariant between runs.
 *
 */
/* eslint-disable @typescript-eslint/no-this-alias -- WPT allows for overriding the this environment for a step but defaults to the Test class */
class Test {
  public static Phases = {
    INITIAL: 0,
    STARTED: 1,
    HAS_RESULT: 2,
    CLEANING: 3,
    COMPLETE: 4,
  } as const;

  public name: string;
  public properties: unknown;
  public phase: (typeof Test.Phases)[keyof typeof Test.Phases];

  public error?: Error;

  // For convenience, expose a promise that resolves once done() is called
  public isDone: Promise<void>;
  private resolve: () => void;

  public constructor(name: string, properties: unknown) {
    this.name = name;
    this.properties = properties;
    this.phase = Test.Phases.INITIAL;

    // eslint-disable-next-line @typescript-eslint/no-invalid-void-type -- void is being used as a valid generic in this context
    const { promise, resolve } = Promise.withResolvers<void>();
    this.isDone = promise;
    this.resolve = resolve;
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
  public step(
    func: UnknownFunc,
    this_obj?: object,
    ...rest: unknown[]
  ): unknown {
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
  public step_func(func: UnknownFunc, this_obj?: object): UnknownFunc {
    const test_this = this;

    if (arguments.length === 1) {
      this_obj = this;
    }

    return function (...params: unknown[]) {
      return test_this.step.call(test_this, func, this_obj, ...params);
    };
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
  public step_func_done(func?: UnknownFunc, this_obj?: object): UnknownFunc {
    const test_this = this;

    if (arguments.length === 1) {
      this_obj = test_this;
    }

    return function (...params: unknown[]) {
      if (func) {
        test_this.step.call(test_this, func, this_obj, ...params);
      }

      test_this.done();
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
  public unreached_func(description?: string): UnknownFunc {
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
  public step_timeout(
    func: UnknownFunc,
    timeout: number,
    ...rest: unknown[]
  ): ReturnType<typeof setTimeout> {
    const test_this = this;

    return setTimeout(
      this.step_func(function () {
        return func.call(test_this, ...rest);
      }),
      timeout
    );
  }

  public done(): void {
    if (this.phase >= Test.Phases.CLEANING) {
      return;
    }

    this.cleanup();
  }

  public cleanup(): void {
    // Actual cleanup support is not yet needed for the WPT modules we support
    this.phase = Test.Phases.COMPLETE;
    this.resolve();
  }
}
/* eslint-enable @typescript-eslint/no-this-alias */

// Singleton object used to pass test state between the runner and the harness functions available
// to the evaled  WPT test code.
class RunnerState {
  // Filename of the current test. Used in error messages.
  public testFileName: string;

  // The worker environment. Used to allow fetching resources in the test.
  public env: Env;

  // Makes test options available from assertion functions
  public options: TestRunnerOptions;

  // List of failed assertions occuring in the test
  public errors: Error[] = [];

  // Promises returned in the test. The test is done once all promises have resolved.
  public promises: Promise<unknown>[] = [];

  public constructor(
    testFileName: string,
    env: Env,
    options: TestRunnerOptions
  ) {
    this.testFileName = testFileName;
    this.env = env;
    this.options = options;
  }

  public async validate(): Promise<void> {
    // Exception handling is set up on every promise in the test function that created it.
    await Promise.all(this.promises);

    const expectedFailures = new Set(this.options.expectedFailures ?? []);
    const unexpectedFailures = [];

    for (const err of this.errors) {
      if (!expectedFailures.delete(err.message)) {
        err.message = sanitize_unpaired_surrogates(err.message);
        console.error(err);
        unexpectedFailures.push(err.message);
      } else if (this.options.verbose) {
        err.message = sanitize_unpaired_surrogates(err.message);
        console.warn('Expected failure: ', err);
      }
    }

    if (unexpectedFailures.length > 0) {
      console.info(
        'The following tests unexpectedly failed:',
        unexpectedFailures
      );
    }

    if (expectedFailures.size > 0) {
      console.info(
        'The following tests were expected to fail but instead succeeded:',
        [...expectedFailures]
      );
    }

    if (unexpectedFailures.length > 0 || expectedFailures.size > 0) {
      throw new Error(
        `${this.testFileName} failed. Please update the test config.`
      );
    }
  }
}

type TestRunnerFn = (callback: TestFn | PromiseTestFn, message: string) => void;
type TestFn = UnknownFunc;
type PromiseTestFn = () => Promise<unknown>;
type ThrowingFn = () => unknown;

declare global {
  /* eslint-disable no-var -- https://www.typescriptlang.org/docs/handbook/release-notes/typescript-3-4.html#type-checking-for-globalthis */
  var state: RunnerState;
  var GLOBAL: { isWindow(): boolean };
  /* eslint-enable no-var */

  function test(func: TestFn, name: string, properties?: unknown): void;
  function done(): undefined;
  function subsetTestByKey(
    _key: string,
    testType: TestRunnerFn,
    testCallback: TestFn | PromiseTestFn,
    testMessage: string
  ): void;
  function promise_test(
    func: PromiseTestFn,
    name: string,
    properties?: unknown
  ): void;
  function async_test(func: TestFn, name: string, properties?: unknown): void;
  function assert_equals(a: unknown, b: unknown, message?: string): void;
  function assert_not_equals(a: unknown, b: unknown, message?: string): void;
  function assert_true(val: unknown, message?: string): void;
  function assert_false(val: unknown, message?: string): void;
  function assert_array_equals(a: unknown, b: unknown, message?: string): void;
  function assert_object_equals(a: unknown, b: unknown, message?: string): void;
  function assert_implements(condition: unknown, description?: string): void;
  function assert_implements_optional(
    condition: unknown,
    description?: string
  ): void;
  function assert_unreached(description?: string): void;
  function assert_throws_js(
    constructor: AssertPredicate,
    func: ThrowingFn,
    description?: string
  ): void;
  function assert_throws_exactly(
    exception: AssertPredicate,
    fn: ThrowingFn,
    description?: string
  ): void;
  function assert_throws_dom(
    type: number | string,
    funcOrConstructor: ThrowingFn | typeof DOMException,
    descriptionOrFunc: string | ThrowingFn,
    maybeDescription?: string
  ): void;
  function assert_not_own_property(
    object: object,
    property_name: string,
    description?: string
  ): void;
}

/**
 * Exception type that represents a failing assert.
 * NOTE: This a custom error type defined by WPT - it's not the same as node:assert's AssertionError
 * @param message - Error message.
 */
declare class AssertionError extends Error {}
function AssertionError(this: AssertionError, message: string): void {
  if (typeof message == 'string') {
    message = sanitize_unpaired_surrogates(message);
  }
  this.message = message;
}

// eslint-disable-next-line  @typescript-eslint/no-unsafe-assignment -- eslint doesn't like "old-style" classes. Code is copied from WPT
AssertionError.prototype = Object.create(Error.prototype);

declare class OptionalFeatureUnsupportedError extends AssertionError {}
function OptionalFeatureUnsupportedError(
  this: OptionalFeatureUnsupportedError,
  message: string
): void {
  AssertionError.call(this, message);
}

// eslint-disable-next-line  @typescript-eslint/no-unsafe-assignment -- eslint doesn't like "old-style" classes. Code is copied from WPT
OptionalFeatureUnsupportedError.prototype = Object.create(
  AssertionError.prototype
);

function code_unit_str(char: string): string {
  return 'U+' + char.charCodeAt(0).toString(16);
}

function sanitize_unpaired_surrogates(str: string): string {
  // Test logs will be exported to XML, so we must escape any characters that
  // are forbidden in an XML CDATA section, namely "[...] the surrogate blocks,
  // FFFE, and FFFF".
  // See https://www.w3.org/TR/REC-xml/#NT-Char

  return str.replace(
    /([\ud800-\udbff]+)(?![\udc00-\udfff])|(^|[^\ud800-\udbff])([\udc00-\udfff]+)/g,
    function (_, low?: string, prefix?: string, high?: string) {
      let output = prefix || ''; // prefix may be undefined
      const string: string = low || high || ''; // only one of these alternates can match

      for (const ch of string) {
        output += code_unit_str(ch);
      }
      return output;
    }
  );
}

// eslint-disable-next-line @typescript-eslint/no-unsafe-assignment,@typescript-eslint/no-unsafe-member-access --  We're just exposing enough stuff for the tests to pass; it's not a perfect match
globalThis.Window = Object.getPrototypeOf(globalThis).constructor;

globalThis.fetch = async (
  input: RequestInfo | URL,
  _init?: RequestInit
  // eslint-disable-next-line @typescript-eslint/require-await -- We are emulating an existing interface that returns a promise
): Promise<Response> => {
  const url =
    input instanceof Request ? input.url.toString() : input.toString();
  const exports: unknown = globalThis.state.env[url];
  const response = new Response();
  // eslint-disable-next-line @typescript-eslint/require-await -- We are emulating an existing interface that returns a promise
  response.json = async (): Promise<unknown> => exports;
  return response;
};

// @ts-expect-error We're just exposing enough stuff for the tests to pass; it's not a perfect match
globalThis.self = globalThis;

globalThis.GLOBAL = {
  isWindow(): boolean {
    return false;
  },
};

globalThis.done = (): undefined => undefined;

globalThis.subsetTestByKey = (
  _key,
  testType,
  testCallback,
  testMessage
): void => {
  // This function is designed to allow selecting only certain tests when
  // running in a browser, by changing the query string. We'll always run
  // all the tests.

  // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression -- We are emulating WPT's existing interface which always passes through the returned value
  return testType(testCallback, testMessage);
};

globalThis.promise_test = (func, name, properties): void => {
  if (!shouldRunTest(name)) {
    return;
  }

  const testCase = new Test(name, properties);
  const promise = testCase.step(func, testCase, testCase);

  if (!(promise instanceof Promise)) {
    // The functions passed to promise_test are expected to return a Promise,
    // but are not required to be async functions. That means they could throw
    // an error immediately when run.

    if (testCase.error) {
      globalThis.state.errors.push(testCase.error);
    } else {
      globalThis.state.errors.push(
        new Error('Unexpected value returned from promise_test')
      );
    }

    return;
  }

  globalThis.state.promises.push(
    promise.catch((err: unknown) => {
      globalThis.state.errors.push(new AggregateError([err], name));
    })
  );
};

globalThis.async_test = (func, name, properties): void => {
  if (!shouldRunTest(name)) {
    return;
  }

  const testCase = new Test(name, properties);
  testCase.step(func, testCase, testCase);

  globalThis.state.promises.push(
    testCase.isDone.then(() => {
      if (testCase.error) {
        globalThis.state.errors.push(testCase.error);
      }
    })
  );
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
  if (!shouldRunTest(name)) {
    return;
  }

  const testCase = new Test(name, properties);
  testCase.step(func, testCase, testCase);
  testCase.done();

  if (testCase.error) {
    globalThis.state.errors.push(testCase.error);
  }
};

globalThis.assert_equals = (a, b, message): void => {
  strictEqual(a, b, message);
};

globalThis.assert_not_equals = (a, b, message): void => {
  notStrictEqual(a, b, message);
};

globalThis.assert_true = (val, message): void => {
  strictEqual(val, true, message);
};

globalThis.assert_false = (val, message): void => {
  strictEqual(val, false, message);
};

globalThis.assert_array_equals = (a, b, message): void => {
  deepStrictEqual(a, b, message);
};

globalThis.assert_object_equals = (a, b, message): void => {
  deepStrictEqual(a, b, message);
};

/**
 * Assert that a feature is implemented, based on a 'truthy' condition.
 *
 * This function should be used to early-exit from tests in which there is
 * no point continuing without support for a non-optional spec or spec
 * feature. For example:
 *
 *     assert_implements(window.Foo, 'Foo is not supported');
 *
 * @param condition The truthy value to test
 * @param [description] Error description for the case that the condition is not truthy.
 */
globalThis.assert_implements = (condition, description): void => {
  ok(!!condition, description);
};

/**
 * Assert that an optional feature is implemented, based on a 'truthy' condition.
 *
 * This function should be used to early-exit from tests in which there is
 * no point continuing without support for an explicitly optional spec or
 * spec feature. For example:
 *
 *     assert_implements_optional(video.canPlayType("video/webm"),
 *                                "webm video playback not supported");
 *
 * @param condition The truthy value to test
 * @param [description] Error description for the case that the condition is not truthy.
 */
globalThis.assert_implements_optional = (condition, description): void => {
  if (!condition) {
    throw new OptionalFeatureUnsupportedError(description ?? '');
  }
};

/**
 * Asserts if called. Used to ensure that a specific code path is
 * not taken e.g. that an error event isn't fired.
 *
 * @param [description] - Description of the condition being tested.
 */
globalThis.assert_unreached = (description): void => {
  ok(false, `Reached unreachable code: ${description ?? 'undefined'}`);
};

/**
 * Assert a JS Error with the expected constructor is thrown.
 *
 * @param constructor The expected exception constructor.
 * @param func Function which should throw.
 * @param [description] Error description for the case that the error is not thrown.
 */
globalThis.assert_throws_js = (constructor, func, description): void => {
  throws(
    () => {
      func.call(this);
    },
    constructor,
    description
  );
};

/**
 * Assert the provided value is thrown.
 *
 * @param exception The expected exception.
 * @param fn Function which should throw.
 * @param [description] Error description for the case that the error is not thrown.
 */
globalThis.assert_throws_exactly = (exception, fn, description): void => {
  try {
    fn.call(this);
  } catch (err) {
    strictEqual(
      err,
      exception,
      description ?? "Thrown exception doesn't match expected value"
    );
    return;
  }

  fail(description ?? 'No exception was thrown');
};

/**
 * Assert a DOMException with the expected type is thrown.
 *
 * There are two ways of calling assert_throws_dom:
 *
 * 1) If the DOMException is expected to come from the current global, the
 * second argument should be the function expected to throw and a third,
 * optional, argument is the assertion description.
 *
 * 2) If the DOMException is expected to come from some other global, the
 * second argument should be the DOMException constructor from that global,
 * the third argument the function expected to throw, and the fourth, optional,
 * argument the assertion description.
 *
 * @param type - The expected exception name or
 * code.  See the `table of names and codes
 * <https://webidl.spec.whatwg.org/#dfn-error-names-table>`_. If a
 * number is passed it should be one of the numeric code values in
 * that table (e.g. 3, 4, etc).  If a string is passed it can
 * either be an exception name (e.g. "HierarchyRequestError",
 * "WrongDocumentError") or the name of the corresponding error
 * code (e.g. "``HIERARCHY_REQUEST_ERR``", "``WRONG_DOCUMENT_ERR``").
 * @param descriptionOrFunc - The function expected to
 * throw (if the exception comes from another global), or the
 * optional description of the condition being tested (if the
 * exception comes from the current global).
 * @param [maybeDescription] - Description of the condition
 * being tested (if the exception comes from another global).
 *
 */
globalThis.assert_throws_dom = (
  _type,
  funcOrConstructor,
  descriptionOrFunc,
  maybeDescription
): void => {
  let constructor: typeof DOMException;
  let func: ThrowingFn;
  let description: string;

  if (funcOrConstructor.name === 'DOMException') {
    constructor = funcOrConstructor as typeof DOMException;
    func = descriptionOrFunc as ThrowingFn;
    description = maybeDescription as string;
  } else {
    // @ts-expect-error This code is copied as is from the WPT harness
    constructor = this.DOMException as typeof DOMException;
    func = funcOrConstructor as ThrowingFn;
    description = descriptionOrFunc as string;
    ok(
      maybeDescription === undefined,
      'Too many args passed to no-constructor version of assert_throws_dom'
    );
  }

  throws(
    () => {
      func.call(this);
    },
    constructor,
    description
  );
};

/**
 * Assert that ``object`` does not have an own property with name ``property_name``.
 *
 * @param object - Object that should not have the given property.
 * @param property_name - Property name to test.
 * @param [description] - Description of the condition being tested.
 */
globalThis.assert_not_own_property = (
  object,
  property_name,
  description
): void => {
  ok(
    !Object.prototype.hasOwnProperty.call(object, property_name),
    `unexpected property ${property_name} is found on object: ` +
      (description ?? '')
  );
};

function shouldRunTest(message: string): boolean {
  if ((globalThis.state.options.skippedTests ?? []).includes(message)) {
    return false;
  }

  if (globalThis.state.options.verbose) {
    console.log('run', message);
  }

  return true;
}

class WPTMetadata {
  // Specifies the Javascript global scopes for the test. (Not currently supported)
  public global: string[] = [];
  // Specifies additional JS scripts to execute.
  public scripts: string[] = [];
  // Adjusts the timeout for the tests in this file. (Not currently supported)
  public timeout?: string;
  // Sets a human-readable title for the entire test file. (Not currently supported.)
  public title?: string;
  // Specifies a variant of this test, which can be used in subsetTestByKey (Not currently supported.)
  public variants: URLSearchParams[] = [];
}

function parseWptMetadata(code: string): WPTMetadata {
  const meta = new WPTMetadata();

  for (const { groups } of code.matchAll(
    /\/\/ META: (?<key>\w+)=(?<value>.+)/g
  )) {
    if (!groups || !groups.key || !groups.value) {
      continue;
    }

    switch (groups.key) {
      case 'global':
        meta.global = groups.value.split(',');
        break;

      case 'script': {
        meta.scripts.push(path.normalize(groups.value));
        break;
      }

      case 'timeout':
        meta.timeout = groups.value;
        break;

      case 'title':
        meta.title = groups.value;
        break;

      case 'variant':
        meta.variants.push(new URLSearchParams(groups.value));
        break;
    }
  }

  return meta;
}

function evalOnce(includedFiles: Set<string>, env: Env, path: string): void {
  if (path === '/common/subset-tests-by-key.js') {
    // The functionality in this file is directly implemented in harness.ts
    return;
  }

  if (includedFiles.has(path)) {
    return;
  }

  if (typeof env[path] != 'string') {
    // We only have access to the files explicitly declared in the .wd-test, not the full WPT
    // checkout, so it's possible for tests to include things we can't load.
    throw new Error(
      `Test file ${path} not found. Update wpt_test.bzl to handle this case.`
    );
  }

  includedFiles.add(path);
  env.unsafe.eval(env[path]);
}

export function createRunner(
  config: TestRunnerConfig,
  allTestFiles: string[]
): (file: string) => TestCase {
  const testsNotFound = new Set(Object.keys(config)).difference(
    new Set(allTestFiles)
  );

  if (testsNotFound.size > 0) {
    throw new Error(
      `Configuration was provided for the following tests that have not been found in the WPT repo: ${[...testsNotFound].join(', ')}`
    );
  }

  // Keeps track of test files which have been included, to avoid loading the same file more
  // than once. Test files are executed in the global scope using unsafeEval, so executing the same
  // file again could cause redefinition errors depending on what is in the file.
  const includedFiles = new Set<string>();

  return (file: string): TestCase => {
    return {
      async test(_: unknown, env: Env): Promise<void> {
        const options = config[file];
        const mainCode = String(env[file]);

        if (!options) {
          throw new Error(
            `Missing test configuration for ${file}. Specify '${file}': {} for default options.`
          );
        }

        if (options.skipAllTests) {
          console.warn(`All tests in ${file} have been skipped.`);
          return;
        }

        globalThis.state = new RunnerState(file, env, options);
        const meta = parseWptMetadata(mainCode);

        for (const script of meta.scripts) {
          evalOnce(includedFiles, env, script);
        }

        evalOnce(includedFiles, env, file);

        await globalThis.state.validate();
      },
    };
  };
}
