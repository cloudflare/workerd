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
  rejects,
  match,
  type AssertPredicate,
} from 'node:assert';

import { type Test } from './test';
import { sanitize_unpaired_surrogates } from './common';

declare global {
  var AssertionError: unknown;

  function assert_equals(a: unknown, b: unknown, message?: string): void;
  function assert_not_equals(a: unknown, b: unknown, message?: string): void;
  function assert_true(val: unknown, message?: string): void;
  function assert_false(val: unknown, message?: string): void;
  function assert_array_equals(
    actual: unknown[],
    expected: unknown[],
    description?: string
  ): void;
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
  function promise_rejects_dom(
    test: Test,
    type: number | string,
    promiseOrConstructor: Promise<unknown> | typeof DOMException,
    descriptionOrPromise: Promise<unknown> | string,
    maybeDescription?: string
  ): Promise<unknown>;

  function assert_not_own_property(
    object: object,
    property_name: string,
    description?: string
  ): void;
  function promise_rejects_js(
    test: Test,
    constructor: typeof Error,
    promise: Promise<unknown>,
    description?: string
  ): Promise<void>;
  function assert_regexp_match(
    actual: string,
    expected: RegExp,
    description?: string
  ): void;
  function assert_greater_than(
    actual: number,
    expected: number,
    description?: string
  ): void;

  function assert_greater_than_equal(
    actual: number,
    expected: number,
    description?: string
  ): void;

  function assert_less_than(
    actual: number,
    expected: number,
    description?: string
  ): void;

  function assert_less_than_equal(
    actual: number,
    expected: number,
    description?: string
  ): void;

  function promise_rejects_exactly(
    test: Test,
    exception: typeof Error,
    promise: Promise<unknown>,
    description?: string
  ): Promise<void>;

  function assert_in_array(
    actual: unknown,
    expected: unknown[],
    description?: string
  ): void;
}

type ThrowingFn = () => unknown;

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

globalThis.AssertionError = AssertionError;

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

/**
 * Assert that ``actual`` and ``expected`` are both arrays, and that the array properties of
 * ``actual`` and ``expected`` are all the same value (as for :js:func:`assert_equals`).
 *
 * @param actual - Test array.
 * @param expected - Array that is expected to contain the same values as ``actual``.
 * @param [description] - Description of the condition being tested.
 */
globalThis.assert_array_equals = (actual, expected, description): void => {
  strictEqual(actual.length, expected.length, description);

  for (let i = 0; i < actual.length; i++) {
    strictEqual(
      Object.prototype.hasOwnProperty.call(actual, i),
      Object.prototype.hasOwnProperty.call(expected, i),
      description
    );
    strictEqual(actual[i], expected[i], description);
  }
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
  type,
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
    constructor = DOMException;
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
    (err: DOMException) => {
      strictEqual(err.constructor, constructor);
      if (typeof type === 'string') {
        strictEqual(err.name, type, description);
      } else {
        // eslint-disable-next-line @typescript-eslint/no-deprecated -- WPT allows tests to check the deprecated 'code' property so we must support this
        strictEqual(err.code, type, description);
      }

      return true;
    },
    `Failed to throw: ${description}`
  );
};

/**
 * Assert that a Promise is rejected with the right DOMException.
 *
 * For the remaining arguments, there are two ways of calling
 * promise_rejects_dom:
 *
 * 1) If the DOMException is expected to come from the current global, the
 * third argument should be the promise expected to reject, and a fourth,
 * optional, argument is the assertion description.
 *
 * 2) If the DOMException is expected to come from some other global, the
 * third argument should be the DOMException constructor from that global,
 * the fourth argument the promise expected to reject, and the fifth,
 * optional, argument the assertion description.
 *
 * @param _test - the `Test` to use for the assertion.
 * @param type - See documentation for
 * `assert_throws_dom <#assert_throws_dom>`_.
 * @param promiseOrConstructor - Either the constructor
 * for the expected exception (if the exception comes from another
 * global), or the promise that's expected to reject (if the
 * exception comes from the current global).
 * @param descriptionOrPromise - Either the
 * promise that's expected to reject (if the exception comes from
 * another global), or the optional description of the condition
 * being tested (if the exception comes from the current global).
 * @param [maybeDescription] - Description of the condition
 * being tested (if the exception comes from another global).
 *
 */
globalThis.promise_rejects_dom = (
  _test,
  type,
  promiseOrConstructor,
  descriptionOrPromise,
  maybeDescription
): Promise<unknown> => {
  let constructor, promise, description;
  if (
    typeof promiseOrConstructor === 'function' &&
    promiseOrConstructor.name === 'DOMException'
  ) {
    constructor = promiseOrConstructor;
    promise = descriptionOrPromise as Promise<unknown>;
    description = maybeDescription as string;
  } else {
    constructor = DOMException;
    promise = promiseOrConstructor as Promise<unknown>;
    description = descriptionOrPromise as string;
    strictEqual(
      maybeDescription,
      undefined,
      'Too many args passed to no-constructor version of promise_rejects_dom, or accidentally explicitly passed undefined'
    );
  }

  return promise
    .then(() => {
      assert_unreached('Should have rejected: ' + description);
    })
    .catch(function (e: unknown) {
      assert_throws_dom(
        type,
        constructor,
        function () {
          throw e;
        },
        description
      );
    });
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

/**
 * Assert that a Promise is rejected with the right ECMAScript exception.
 *
 * @param test - the `Test` to use for the assertion.
 * @param constructor - The expected exception constructor.
 * @param promise - The promise that's expected to
 * reject with the given exception.
 * @param [description] Error message to add to assert in case of
 *                               failure.
 */
globalThis.promise_rejects_js = async (
  _test,
  constructor,
  promise,
  description
): Promise<void> => {
  return rejects(promise, constructor, description);
};

/**
 * Assert that ``actual`` matches the RegExp ``expected``.
 *
 * @param actual - Test string.
 * @param expected - RegExp ``actual`` must match.
 * @param [description] - Description of the condition being tested.
 */
globalThis.assert_regexp_match = (actual, expected, description): void => {
  match(actual, expected, description);
};

/**
 * Assert that ``actual`` is a number greater than ``expected``.
 *
 * @param actual - Test value.
 * @param expected - Number that ``actual`` must be greater than.
 * @param [description] - Description of the condition being tested.
 */
globalThis.assert_greater_than = (actual, expected, description): void => {
  ok(actual > expected, description);
};

/**
 * Assert that ``actual`` is a number greater than or equal to ``expected``.
 *
 * @param actual - Test value.
 * @param expected - Number that ``actual`` must be greater than or equal to.
 * @param [description] - Description of the condition being tested.
 */
globalThis.assert_greater_than_equal = (
  actual,
  expected,
  description
): void => {
  ok(actual >= expected, description);
};

/**
 * Assert that ``actual`` is a number less than ``expected``.
 *
 * @param actual - Test value.
 * @param expected - Number that ``actual`` must be less than.
 * @param [description] - Description of the condition being tested.
 */
globalThis.assert_less_than = (actual, expected, description): void => {
  ok(actual < expected, description);
};

/**
 * Assert that ``actual`` is a number less than or equal to ``expected``.
 *
 * @param actual - Test value.
 * @param expected - Number that ``actual`` must be less than or equal to.
 * @param [description] - Description of the condition being tested.
 */
globalThis.assert_less_than_equal = (actual, expected, description): void => {
  ok(actual <= expected, description);
};

/**
 * Assert that a Promise is rejected with the provided value.
 *
 * @param test - the `Test` to use for the assertion.
 * @param exception - The expected value of the rejected promise.
 * @param promise - The promise that's expected to
 * reject.
 * @param [description] Error message to add to assert in case of
 *                               failure.
 */
globalThis.promise_rejects_exactly = (
  _test,
  exception,
  promise,
  description
): Promise<void> => {
  return promise
    .then(() => {
      assert_unreached(`Should have rejected: ${description}`);
    })
    .catch((exc: unknown) => {
      assert_throws_exactly(
        exception,
        () => {
          throw exc;
        },
        description
      );
    });
};

/**
 * Assert that ``expected`` is an array and ``actual`` is one of the members.
 * This is implemented using ``indexOf``, so doesn't handle NaN or ±0 correctly.
 *
 * @param actual - Test value.
 * @param expected - An array that ``actual`` is expected to
 * be a member of.
 * @param [description] - Description of the condition being tested.
 */
globalThis.assert_in_array = (actual, expected, description): void => {
  notStrictEqual(
    expected.indexOf(actual),
    -1,
    `assert_in_array ${description}: value ${actual} not in array ${expected}`
  );
};
