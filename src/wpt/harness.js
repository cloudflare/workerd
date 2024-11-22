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
  throws,
  ok,
} from 'node:assert';

globalThis.AssertionError = function AssertionError(message) {
  this.message = message;
};

function OptionalFeatureUnsupportedError(message) {
  AssertionError.call(this, message);
}
OptionalFeatureUnsupportedError.prototype = Object.create(
  AssertionError.prototype
);

globalThis.fetch = async (url) => {
  const { default: data } = await import(url);
  return {
    async json() {
      return data;
    },
  };
};

globalThis.GLOBAL = { isWindow: () => false };

globalThis.done = () => undefined;

globalThis.subsetTestByKey = (_key, testType, testCallback, testMessage) => {
  // This function is designed to allow selecting only certain tests when
  // running in a browser, by changing the query string. We'll always run
  // all the tests.

  return testType(testCallback, testMessage);
};

globalThis.promise_test = (callback, message) => {
  if (!shouldRunTest(message)) {
    return;
  }

  try {
    globalThis.promises.push(callback());
  } catch (err) {
    globalThis.errors.push(new AggregateError([err], message));
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

globalThis.assert_implements = (condition, message) => {
  ok(!!condition, message);
};

globalThis.assert_implements_optional = (condition, description) => {
  if (!condition) {
    throw new OptionalFeatureUnsupportedError(description);
  }
};

// Implementation is taken from Node.js WPT harness.
// Ref: https://github.com/nodejs/node/blob/8807549ed9f6eaf6842ae56b8ac55ab385951636/test/fixtures/wpt/resources/testharness.js#L2046
globalThis.assert_throws_js = (
  constructor,
  func,
  description,
  assertion_type
) => {
  try {
    func.call(this);
    ok(false, assertion_type, description, '${func} did not throw', {
      func: func,
    });
  } catch (e) {
    if (e instanceof AssertionError) {
      throw e;
    }

    // Basic sanity-checks on the thrown exception.
    ok(
      typeof e === 'object',
      assertion_type,
      description,
      '${func} threw ${e} with type ${type}, not an object',
      { func: func, e: e, type: typeof e }
    );

    ok(
      e !== null,
      assertion_type,
      description,
      '${func} threw null, not an object',
      { func: func }
    );

    // Basic sanity-check on the passed-in constructor
    ok(
      typeof constructor == 'function',
      assertion_type,
      description,
      '${constructor} is not a constructor',
      { constructor: constructor }
    );
    var obj = constructor;
    while (obj) {
      if (typeof obj === 'function' && obj.name === 'Error') {
        break;
      }
      obj = Object.getPrototypeOf(obj);
    }
    ok(
      obj != null,
      assertion_type,
      description,
      '${constructor} is not an Error subtype',
      { constructor: constructor }
    );

    // And checking that our exception is reasonable
    ok(
      e.constructor === constructor && e.name === constructor.name,
      assertion_type,
      description,
      '${func} threw ${actual} (${actual_name}) expected instance of ${expected} (${expected_name})',
      {
        func: func,
        actual: e,
        actual_name: e.name,
        expected: constructor,
        expected_name: constructor.name,
      }
    );
  }
};

globalThis.assert_throws_exactly = (expected, fn, message) => {
  try {
    fn();
  } catch (actual) {
    deepStrictEqual(actual, expected, message);
  }
};

globalThis.assert_throws_dom = (name, fn, message) => {
  throws(
    fn,
    (err) => {
      ok(err instanceof DOMException, message);
      deepStrictEqual(err.name, name, message);
      return true;
    },
    message
  );
};

globalThis.test = (callback, message) => {
  if (!shouldRunTest(message)) {
    return;
  }

  try {
    callback();
  } catch (err) {
    globalThis.errors.push(new AggregateError([err], message));
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
  globalThis.promises = [];
  globalThis.testOptions = options;
}

function sanitizeMessage(message) {
  // Test logs will be exported to XML, so we must escape any characters that
  // are forbidden in an XML CDATA section, namely "[...] the surrogate blocks,
  // FFFE, and FFFF".
  // See https://www.w3.org/TR/REC-xml/#NT-Char
  return message.replace(
    /[\u{D800}-\u{DFFF}\u{FFFE}\u{FFFF}]/gu,
    (char) => '\\u' + char.charCodeAt().toString(16).padStart(4, '0')
  );
}

async function validate(options) {
  await Promise.all(globalThis.promises);

  const expectedFailures = new Set(options.expectedFailures ?? []);

  for (const err of globalThis.errors) {
    const sanitizedError = new AggregateError(
      err.errors,
      sanitizeMessage(err.message)
    );
    if (!expectedFailures.delete(err.message)) {
      console.error(sanitizedError);
      throw new Error('Test failed');
    }
  }

  if (expectedFailures.size > 0) {
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
      await validate(options);
    },
  };
}
