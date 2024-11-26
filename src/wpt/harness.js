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
  AssertionError,
  strictEqual,
  notStrictEqual,
  deepStrictEqual,
  ok,
  throws,
} from 'node:assert';

function OptionalFeatureUnsupportedError(message) {
  AssertionError.call(this, message);
}
OptionalFeatureUnsupportedError.prototype = Object.create(
  AssertionError.prototype
);

globalThis.Window = Object.getPrototypeOf(globalThis).constructor;

globalThis.fetch = async (url) => {
  const { default: data } = await import(url);
  return {
    async json() {
      return data;
    },
  };
};

globalThis.self = globalThis;
globalThis.GLOBAL = {
  isWindow() {
    return false;
  },
};

globalThis.done = () => undefined;

globalThis.subsetTestByKey = (_key, testType, testCallback, testMessage) => {
  // This function is designed to allow selecting only certain tests when
  // running in a browser, by changing the query string. We'll always run
  // all the tests.

  return testType(testCallback, testMessage);
};

globalThis.promise_test = async (func, name, properties) => {
  if (typeof func !== 'function') {
    properties = name;
    name = func;
    func = null;
  }

  if (!shouldRunTest(name)) {
    return;
  }

  try {
    await func.call(this);
  } catch (err) {
    globalThis.errors.push(new AggregateError([err], name));
  }
};

globalThis.assert_equals = (a, b, message) => {
  strictEqual(a, b, message);
};

globalThis.assert_not_equals = (a, b, message) => {
  notStrictEqual(a, b, message);
};

globalThis.assert_true = (val, message) => {
  strictEqual(val, true, message);
};

globalThis.assert_false = (val, message) => {
  strictEqual(val, false, message);
};

globalThis.assert_array_equals = (a, b, message) => {
  deepStrictEqual(a, b, message);
};

globalThis.assert_object_equals = (a, b, message) => {
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
globalThis.assert_implements = (condition, description) => {
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
globalThis.assert_implements_optional = (condition, description) => {
  if (!condition) {
    throw new OptionalFeatureUnsupportedError(description);
  }
};

/**
 * Asserts if called. Used to ensure that a specific code path is
 * not taken e.g. that an error event isn't fired.
 *
 * @param {string} [description] - Description of the condition being tested.
 */
globalThis.assert_unreached = (description) => {
  ok(false, `Reached unreachable code: ${description}`);
};

/**
 * Assert a JS Error with the expected constructor is thrown.
 *
 * @param {object} constructor The expected exception constructor.
 * @param {Function} func Function which should throw.
 * @param {string} [description] Error description for the case that the error is not thrown.
 */
globalThis.assert_throws_js = (constructor, func, description) => {
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
globalThis.assert_throws_exactly = (exception, fn, description) => {
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
  type,
  funcOrConstructor,
  descriptionOrFunc,
  maybeDescription
) => {
  let constructor, func, description;
  if (funcOrConstructor.name === 'DOMException') {
    constructor = funcOrConstructor;
    func = descriptionOrFunc;
    description = maybeDescription;
  } else {
    constructor = this.DOMException;
    func = funcOrConstructor;
    description = descriptionOrFunc;
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
 * @param {TestFunction} func - Test function. This is executed
 * immediately. If it returns without error, the test status is
 * set to ``PASS``. If it throws an :js:class:`AssertionError`, or
 * any other exception, the test status is set to ``FAIL``
 * (typically from an `assert` function).
 * @param {String} name - Test name. This must be unique in a
 * given file and must be invariant between runs.
 */
globalThis.test = (func, name) => {
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

function shouldRunTest(message) {
  if ((globalThis.testOptions.skippedTests ?? []).includes(message)) {
    return false;
  }

  if (globalThis.testOptions.verbose) {
    console.log('run', message);
  }

  return true;
}

function prepare(options) {
  globalThis.errors = [];
  globalThis.testOptions = options;
}

function sanitizeMessage(message) {
  // Test logs will be exported to XML, so we must escape any characters that
  // are forbidden in an XML CDATA section, namely "[...] the surrogate blocks,
  // FFFE, and FFFF".
  // See https://www.w3.org/TR/REC-xml/#NT-Char
  return message.replace(
    /([\ud800-\udbff]+)(?![\udc00-\udfff])|(^|[^\ud800-\udbff])([\udc00-\udfff]+)/g,
    function (_, low, prefix, high) {
      let output = prefix || ''; // prefix may be undefined
      const string = low || high; // only one of these alternates can match
      for (let i = 0; i < string.length; i++) {
        output += 'U+' + string[i].charCodeAt(0).toString(16);
      }
      return output;
    }
  );
}

function validate(testFileName, options) {
  const expectedFailures = new Set(options.expectedFailures ?? []);

  let failing = false;
  for (const err of globalThis.errors) {
    if (!expectedFailures.delete(err.message)) {
      err.message = sanitizeMessage(err.message);
      console.error(err);
      failing = true;
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

export function run(file, options = {}) {
  return {
    async test() {
      prepare(options);
      await import(file);
      validate(file, options);
    },
  };
}
