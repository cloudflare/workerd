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
  type AssertPredicate,
} from 'node:assert';

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

type TestRunnerFn = (callback: TestFn | PromiseTestFn, message: string) => void;
type TestFn = () => void;
type PromiseTestFn = () => Promise<void>;
type ThrowingFn = () => unknown;

declare global {
  /* eslint-disable no-var -- https://www.typescriptlang.org/docs/handbook/release-notes/typescript-3-4.html#type-checking-for-globalthis */
  var errors: Error[];
  var testOptions: TestRunnerOptions;
  var GLOBAL: { isWindow(): boolean };
  var env: Env;
  var promises: { [name: string]: Promise<void> };
  /* eslint-enable no-var */

  function test(func: TestFn, name: string): void;
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
}

/**
 * @class
 * Exception type that represents a failing assert.
 * NOTE: This a custom error type defined by WPT - it's not the same as node:assert's AssertionError
 * @param {string} message - Error message.
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
  const exports: unknown = env[url];
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

globalThis.promise_test = (func, name, _properties): void => {
  if (!shouldRunTest(name)) {
    return;
  }

  try {
    globalThis.promises[name] = func.call(this);
  } catch (err) {
    globalThis.errors.push(new AggregateError([err], name));
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
 * @param {object} condition The truthy value to test
 * @param {string} [description] Error description for the case that the condition is not truthy.
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
 * @param {object} condition The truthy value to test
 * @param {string} [description] Error description for the case that the condition is not truthy.
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
 * @param {string} [description] - Description of the condition being tested.
 */
globalThis.assert_unreached = (description): void => {
  ok(false, `Reached unreachable code: ${description ?? 'undefined'}`);
};

/**
 * Assert a JS Error with the expected constructor is thrown.
 *
 * @param {object} constructor The expected exception constructor.
 * @param {Function} func Function which should throw.
 * @param {string} [description] Error description for the case that the error is not thrown.
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
 * @param {value} exception The expected exception.
 * @param {Function} fn Function which should throw.
 * @param {string} [description] Error description for the case that the error is not thrown.
 */
globalThis.assert_throws_exactly = (exception, fn, description): void => {
  throws(
    () => {
      fn.call(this);
    },
    exception,
    description
  );
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
 * @param {number|string} type - The expected exception name or
 * code.  See the `table of names and codes
 * <https://webidl.spec.whatwg.org/#dfn-error-names-table>`_. If a
 * number is passed it should be one of the numeric code values in
 * that table (e.g. 3, 4, etc).  If a string is passed it can
 * either be an exception name (e.g. "HierarchyRequestError",
 * "WrongDocumentError") or the name of the corresponding error
 * code (e.g. "``HIERARCHY_REQUEST_ERR``", "``WRONG_DOCUMENT_ERR``").
 * @param {Function} descriptionOrFunc - The function expected to
 * throw (if the exception comes from another global), or the
 * optional description of the condition being tested (if the
 * exception comes from the current global).
 * @param {string} [maybeDescription] - Description of the condition
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
 * Create a synchronous test
 *
 * @param {TestFn} func - Test function. This is executed
 * immediately. If it returns without error, the test status is
 * set to ``PASS``. If it throws an :js:class:`AssertionError`, or
 * any other exception, the test status is set to ``FAIL``
 * (typically from an `assert` function).
 * @param {String} name - Test name. This must be unique in a
 * given file and must be invariant between runs.
 */
globalThis.test = (func, name): void => {
  if (!shouldRunTest(name)) {
    return;
  }

  try {
    func.call(this);
  } catch (err) {
    globalThis.errors.push(new AggregateError([err], name));
  }
};

globalThis.errors = [];

function shouldRunTest(message: string): boolean {
  if ((globalThis.testOptions.skippedTests ?? []).includes(message)) {
    return false;
  }

  if (globalThis.testOptions.verbose) {
    console.log('run', message);
  }

  return true;
}

function prepare(env: Env, options: TestRunnerOptions): void {
  globalThis.errors = [];
  globalThis.testOptions = options;
  globalThis.env = env;
  globalThis.promises = {};
}

async function validate(
  testFileName: string,
  options: TestRunnerOptions
): Promise<void> {
  for (const [name, promise] of Object.entries(globalThis.promises)) {
    try {
      await promise;
    } catch (err) {
      globalThis.errors.push(new AggregateError([err], name));
    }
  }

  const expectedFailures = new Set(options.expectedFailures ?? []);

  let failing = false;
  for (const err of globalThis.errors) {
    if (!expectedFailures.delete(err.message)) {
      err.message = sanitize_unpaired_surrogates(err.message);
      console.error(err);
      failing = true;
    } else if (options.verbose) {
      err.message = sanitize_unpaired_surrogates(err.message);
      console.warn('Expected failure: ', err);
    }
  }

  if (failing) {
    throw new Error(`${testFileName} failed`);
  }

  if (expectedFailures.size > 0) {
    console.info('Expected failures', expectedFailures);
    throw new Error(
      'Expected failures but test succeeded. Please update the test config file.'
    );
  }
}

export function run(config: TestRunnerConfig, file: string): TestCase {
  const options = config[file] ?? {};

  return {
    async test(_: unknown, env: Env): Promise<void> {
      if (options.skipAllTests) {
        console.warn(`All tests in ${file} have been skipped.`);
        return;
      }

      prepare(env, options);
      if (typeof env[file] !== 'string') {
        throw new Error(`Unable to run ${file}. Code is not a string`);
      }
      env.unsafe.eval(env[file]);
      await validate(file, options);
    },
  };
}
