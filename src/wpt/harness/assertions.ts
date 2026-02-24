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
  var AssertionError: new (message: string) => Error;

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
  function assert_throws_quotaexceedederror(
    func: ThrowingFn,
    description?: string
  ): void;
  function promise_rejects_dom(
    test: Test,
    type: number | string,
    promiseOrConstructor: Promise<unknown> | typeof DOMException,
    descriptionOrPromise: Promise<unknown> | string,
    maybeDescription?: string
  ): Promise<unknown>;

  function assert_own_property(
    object: object,
    property_name: string | symbol,
    description?: string
  ): void;
  function assert_not_own_property(
    object: object,
    property_name: string | symbol,
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

// Maps legacy DOMException code names (e.g. "INVALID_ACCESS_ERR") to modern names
// (e.g. "InvalidAccessError"). Matches the upstream WPT testharness.js.
const codename_name_map: Record<string, string> = {
  INDEX_SIZE_ERR: 'IndexSizeError',
  HIERARCHY_REQUEST_ERR: 'HierarchyRequestError',
  WRONG_DOCUMENT_ERR: 'WrongDocumentError',
  INVALID_CHARACTER_ERR: 'InvalidCharacterError',
  NO_MODIFICATION_ALLOWED_ERR: 'NoModificationAllowedError',
  NOT_FOUND_ERR: 'NotFoundError',
  NOT_SUPPORTED_ERR: 'NotSupportedError',
  INUSE_ATTRIBUTE_ERR: 'InUseAttributeError',
  INVALID_STATE_ERR: 'InvalidStateError',
  SYNTAX_ERR: 'SyntaxError',
  INVALID_MODIFICATION_ERR: 'InvalidModificationError',
  NAMESPACE_ERR: 'NamespaceError',
  INVALID_ACCESS_ERR: 'InvalidAccessError',
  TYPE_MISMATCH_ERR: 'TypeMismatchError',
  SECURITY_ERR: 'SecurityError',
  NETWORK_ERR: 'NetworkError',
  ABORT_ERR: 'AbortError',
  URL_MISMATCH_ERR: 'URLMismatchError',
  QUOTA_EXCEEDED_ERR: 'QuotaExceededError',
  TIMEOUT_ERR: 'TimeoutError',
  INVALID_NODE_TYPE_ERR: 'InvalidNodeTypeError',
  DATA_CLONE_ERR: 'DataCloneError',
};

// Maps modern DOMException names to their legacy numeric codes.
const name_code_map: Record<string, number> = {
  IndexSizeError: 1,
  HierarchyRequestError: 3,
  WrongDocumentError: 4,
  InvalidCharacterError: 5,
  NoModificationAllowedError: 7,
  NotFoundError: 8,
  NotSupportedError: 9,
  InUseAttributeError: 10,
  InvalidStateError: 11,
  SyntaxError: 12,
  InvalidModificationError: 13,
  NamespaceError: 14,
  InvalidAccessError: 15,
  TypeMismatchError: 17,
  SecurityError: 18,
  NetworkError: 19,
  AbortError: 20,
  URLMismatchError: 21,
  QuotaExceededError: 22,
  TimeoutError: 23,
  InvalidNodeTypeError: 24,
  DataCloneError: 25,
  // Modern exceptions with code 0
  EncodingError: 0,
  NotReadableError: 0,
  UnknownError: 0,
  ConstraintError: 0,
  DataError: 0,
  TransactionInactiveError: 0,
  ReadOnlyError: 0,
  VersionError: 0,
  OperationError: 0,
  NotAllowedError: 0,
  OptOutError: 0,
};

const code_name_map: Record<number, string> = {};
for (const key in name_code_map) {
  if ((name_code_map[key] as number) > 0) {
    code_name_map[name_code_map[key] as number] = key;
  }
}

// Helper for assert_throws_dom_impl assertions. Throws a WPT AssertionError
// (not node:assert's AssertionError) so instanceof checks work correctly.
function assert_dom(
  condition: boolean,
  assertion_type: string,
  description: string | undefined,
  message: string
): void {
  if (condition) return;
  const prefix = description ? `${description}: ` : '';
  throw new AssertionError(`${assertion_type}: ${prefix}${message}`);
}

/**
 * Internal implementation of assert_throws_dom, matching the upstream WPT
 * testharness.js assert_throws_dom_impl. Uses its own try/catch rather than
 * node:assert.throws() so that legacy DOMException code names are mapped to
 * modern names and error properties are checked individually with clear
 * messages.
 */
function assert_throws_dom_impl(
  type: number | string,
  func: ThrowingFn,
  description: string,
  assertion_type: string,
  constructor: typeof DOMException
): void {
  try {
    func();
    assert_dom(false, assertion_type, description, 'function did not throw');
  } catch (e) {
    if (e instanceof AssertionError) {
      throw e;
    }

    // Basic sanity-checks on the thrown exception.
    assert_dom(
      typeof e === 'object',
      assertion_type,
      description,
      `thrown value is not an object (got ${typeof e})`
    );
    assert_dom(e !== null, assertion_type, description, 'thrown value is null');
    assert_dom(
      typeof type === 'number' || typeof type === 'string',
      assertion_type,
      description,
      'type is not a number or string'
    );

    const required_props: Record<string, unknown> = {};
    let name: string;

    if (typeof type === 'number') {
      assert_dom(
        type in code_name_map,
        assertion_type,
        description,
        `Test bug: unrecognized DOMException code "${type}" passed to ${assertion_type}()`
      );
      name = code_name_map[type] as string;
      required_props['code'] = type;
    } else {
      // Map legacy code names (e.g. "INVALID_ACCESS_ERR") to modern names
      name =
        type in codename_name_map ? (codename_name_map[type] as string) : type;
      assert_dom(
        name in name_code_map,
        assertion_type,
        description,
        `Test bug: unrecognized DOMException code name or name "${type}" passed to ${assertion_type}()`
      );
      required_props['code'] = name_code_map[name];
    }

    if (
      required_props['code'] === 0 ||
      ('name' in (e as object) &&
        (e as { name: string }).name !==
          (e as { name: string }).name.toUpperCase() &&
        (e as { name: string }).name !== 'DOMException')
    ) {
      // New style exception: also test the name property.
      required_props['name'] = name;
    }

    for (const prop in required_props) {
      assert_dom(
        prop in (e as object) &&
          (e as Record<string, unknown>)[prop] == required_props[prop],
        assertion_type,
        description,
        `thrown exception is not a DOMException ${type}: property ${prop} is equal to ${(e as Record<string, unknown>)[prop]}, expected ${required_props[prop]}`
      );
    }

    // Check that the exception is from the right global. This check is last
    // so more specific, and more informative, checks on the properties can
    // happen in case a totally incorrect exception is thrown.
    assert_dom(
      (e as object).constructor === constructor,
      assertion_type,
      description,
      'thrown exception from the wrong global'
    );
  }
}

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

  assert_throws_dom_impl(
    type,
    func,
    description,
    'assert_throws_dom',
    constructor
  );
};

/**
 * Assert a QuotaExceededError DOMException is thrown.
 *
 * This is a convenience wrapper around assert_throws_dom for the common case
 * of checking for QuotaExceededError.
 *
 * @param func - Function which should throw.
 * @param [description] - Description of the condition being tested.
 */
globalThis.assert_throws_quotaexceedederror = (func, description): void => {
  assert_throws_dom('QuotaExceededError', func, description ?? '');
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
 * Assert that ``object`` has an own property with name ``property_name``.
 *
 * @param object - Object that should have the given property.
 * @param property_name - Property name to test.
 * @param [description] - Description of the condition being tested.
 */
globalThis.assert_own_property = (object, property_name, description): void => {
  ok(
    Object.prototype.hasOwnProperty.call(object, property_name),
    `expected property ${String(property_name)} missing on object: ` +
      (description ?? '')
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
    `unexpected property ${String(property_name)} is found on object: ` +
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
