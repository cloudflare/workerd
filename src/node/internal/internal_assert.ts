// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Adapted from Deno and Node.js:
// Copyright 2018-2022 the Deno authors. All rights reserved. MIT license.
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
/* eslint-disable */

import {
  AssertionError,
  AssertionErrorConstructorOptions,
} from 'node-internal:internal_assertionerror';

import {
  diffstr,
  diff,
  buildMessage,
} from 'node-internal:internal_diffs';

import {
  isDeepStrictEqual,
} from 'node-internal:internal_comparisons';

import {
  ERR_AMBIGUOUS_ARGUMENT,
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
  ERR_INVALID_RETURN_VALUE,
  ERR_MISSING_ARGS,
} from 'node-internal:internal_errors';

interface ExtendedAssertionErrorConstructorOptions
  extends AssertionErrorConstructorOptions {
  generatedMessage?: boolean;
}

function createAssertionError(
  options: ExtendedAssertionErrorConstructorOptions,
): AssertionError {
  const error = new AssertionError(options);
  if (options.generatedMessage) {
    (error as any).generatedMessage = true;
  }
  return error;
}

function assert(actual: unknown, message?: string | Error): asserts actual {
  if (arguments.length === 0) {
    throw new AssertionError({
      message: "No value argument passed to `assert.ok()`",
    });
  }
  if (!actual) {
    throw new AssertionError({
      message,
      actual,
      expected: true
    } as AssertionErrorConstructorOptions );
  }
}
export const ok = assert;

export function throws(
  fn: () => void,
  error?: RegExp | Function | Error,
  message?: string,
) {
  // Check arg types
  if (typeof fn !== "function") {
    throw new ERR_INVALID_ARG_TYPE("fn", "function", fn);
  }
  if (
    typeof error === "object" && error !== null &&
    Object.getPrototypeOf(error) === Object.prototype &&
    Object.keys(error).length === 0
  ) {
    // error is an empty object
    throw new ERR_INVALID_ARG_VALUE(
      "error",
      error,
      "may not be an empty object",
    );
  }
  if (typeof message === "string") {
    if (
      !(error instanceof RegExp) && typeof error !== "function" &&
      !(error instanceof Error) && typeof error !== "object"
    ) {
      throw new ERR_INVALID_ARG_TYPE("error", [
        "Function",
        "Error",
        "RegExp",
        "Object",
      ], error);
    }
  } else {
    if (
      typeof error !== "undefined" && typeof error !== "string" &&
      !(error instanceof RegExp) && typeof error !== "function" &&
      !(error instanceof Error) && typeof error !== "object"
    ) {
      throw new ERR_INVALID_ARG_TYPE("error", [
        "Function",
        "Error",
        "RegExp",
        "Object",
      ], error);
    }
  }

  // Checks test function
  try {
    fn();
  } catch (e) {
    if (
      validateThrownError(e, error, message, {
        operator: throws,
      })
    ) {
      return;
    }
  }
  if (message) {
    let msg = `Missing expected exception: ${message}`;
    if (typeof error === "function" && error?.name) {
      msg = `Missing expected exception (${error.name}): ${message}`;
    }
    throw new AssertionError({
      message: msg,
      operator: "throws",
      actual: undefined,
      expected: error,
    });
  } else if (typeof error === "string") {
    // Use case of throws(fn, message)
    throw new AssertionError({
      message: `Missing expected exception: ${error}`,
      operator: "throws",
      actual: undefined,
      expected: undefined,
    });
  } else if (typeof error === "function" && error?.prototype !== undefined) {
    throw new AssertionError({
      message: `Missing expected exception (${error.name}).`,
      operator: "throws",
      actual: undefined,
      expected: error,
    });
  } else {
    throw new AssertionError({
      message: "Missing expected exception.",
      operator: "throws",
      actual: undefined,
      expected: error,
    });
  }
}

export function doesNotThrow(
  fn: () => void,
  message?: string,
): void;
export function doesNotThrow(
  fn: () => void,
  error?: Function,
  message?: string | Error,
): void;
export function doesNotThrow(
  fn: () => void,
  error?: RegExp,
  message?: string,
): void;
export function doesNotThrow(
  fn: () => void,
  expected?: Function | RegExp | string,
  message?: string | Error,
) {
  // Check arg type
  if (typeof fn !== "function") {
    throw new ERR_INVALID_ARG_TYPE("fn", "function", fn);
  } else if (
    !(expected instanceof RegExp) && typeof expected !== "function" &&
    typeof expected !== "string" && typeof expected !== "undefined"
  ) {
    throw new ERR_INVALID_ARG_TYPE("expected", ["Function", "RegExp"], fn);
  }

  // Checks test function
  try {
    fn();
  } catch (e) {
    gotUnwantedException(e, expected, message, doesNotThrow);
  }
}

export function equal(
  actual: unknown,
  expected: unknown,
  message?: string | Error,
) {
  return strictEqual(actual, expected, message);
}

export function notEqual(
  actual: unknown,
  expected: unknown,
  message?: string | Error,
) {
  return notStrictEqual(actual, expected, message);
}

export function strictEqual(
  actual: unknown,
  expected: unknown,
  message?: string | Error,
) {
  if (arguments.length < 2) {
    throw new ERR_MISSING_ARGS("actual", "expected");
  }

  if (Object.is(actual, expected)) {
    return;
  }

  if (message) {
    message = `${message}`;
  } else {
    // TODO(soon): Implement inspect
    // const actualString = inspect(actual);
    // const expectedString = inspect(expected);
    const actualString = `${actual}`;
    const expectedString = `${expected}`;

    if (actualString === expectedString) {
      const withOffset = actualString
        .split("\n")
        .map((l) => `    ${l}`)
        .join("\n");
      message =
        `Values have the same structure but are not reference-equal:\n\n${
          withOffset
        }\n`;
    } else {
      try {
        const stringDiff = (typeof actual === "string") &&
          (typeof expected === "string");
        const diffResult = stringDiff
          ? diffstr(actual as string, expected as string)
          : diff(actualString.split("\n"), expectedString.split("\n"));
        const diffMsg = buildMessage(diffResult, { stringDiff }).join("\n");
        message = `Values are not strictly equal:\n${diffMsg}`;
      } catch {
        message = '\n$[Cannot display] + \n\n';
      }
    }
  }

  throw new AssertionError({
    message,
    actual,
    expected,
    operator: 'strictEqual',
  });
}

export function notStrictEqual(
  actual: unknown,
  expected: unknown,
  message?: string | Error,
) {
  if (arguments.length < 2) {
    throw new ERR_MISSING_ARGS("actual", "expected");
  }

  if (!Object.is(actual, expected)) {
    return;
  }

  if (message) {
    message = `${message}`;
  } else {
    message = 'Expected actual to be strictly unequal to expected';
  }

  throw new AssertionError({
    message,
    actual,
    expected,
    operator: 'notStrictEqual',
  });
}

export function deepEqual(
  actual: unknown,
  expected: unknown,
  message?: string | Error,
) {
  return deepStrictEqual(actual, expected, message);
}

export function notDeepEqual(
  actual: unknown,
  expected: unknown,
  message?: string | Error,
) {
  return notDeepStrictEqual(actual, expected, message);
}

export function deepStrictEqual(
  actual: unknown,
  expected: unknown,
  message?: string | Error,
) {
  if (arguments.length < 2) {
    throw new ERR_MISSING_ARGS("actual", "expected");
  }

  if (isDeepStrictEqual(actual, expected)) {
    return;
  }

  if (message) {
    message = `${message}`;
  } else {
    message = 'Expected actual to be deeply strictly equal to expected';
  }

  throw new AssertionError({
    message,
    actual,
    expected,
    operator: 'deepStrictEqual',
  });
}

export function notDeepStrictEqual(
  actual: unknown,
  expected: unknown,
  message?: string | Error,
) {
  if (arguments.length < 2) {
    throw new ERR_MISSING_ARGS("actual", "expected");
  }

  if (isDeepStrictEqual(actual, expected)) {
    if (message) {
      message = `${message}`;
    } else {
      message = 'Expected actual to not be deeply strictly equal to expected';
    }

    throw new AssertionError({
      message,
      actual,
      expected,
      operator: 'notDeepStrictEqual',
    });
  }
}

export function fail(message?: string | Error): never {
  if (typeof message === "string" || message == null) {
    throw createAssertionError({
      message: message ?? "Failed",
      operator: "fail",
      generatedMessage: message == null,
    });
  } else {
    throw message;
  }
}

export function match(actual: string, regexp: RegExp, message?: string | Error) {
  if (arguments.length < 2) {
    throw new ERR_MISSING_ARGS("actual", "regexp");
  }
  if (!(regexp instanceof RegExp)) {
    throw new ERR_INVALID_ARG_TYPE("regexp", "RegExp", regexp);
  }

  if (!regexp.test(actual)) {
    if (!message) {
      message = `actual: "${actual}" expected to match: "${regexp}"`;
    } else {
      message = `${message}`;
    }
    throw new AssertionError({
      message,
      actual,
      expected: regexp,
      operator: 'match',
    });
  }
}

export function doesNotMatch(
  string: string,
  regexp: RegExp,
  message?: string | Error,
) {
  if (arguments.length < 2) {
    throw new ERR_MISSING_ARGS("string", "regexp");
  }
  if (!(regexp instanceof RegExp)) {
    throw new ERR_INVALID_ARG_TYPE("regexp", "RegExp", regexp);
  }
  if (typeof string !== "string") {
    if (message instanceof Error) {
      throw message;
    }
    throw new AssertionError({
      message: message ||
        `The "string" argument must be of type string. Received type ${typeof string} (${
          // TODO(soon): Implement inspect
          // inspect(string)
          string
        })`,
      actual: string,
      expected: regexp,
      operator: "doesNotMatch",
    });
  }

  if (regexp.test(string)) {
    if (!message) {
      message = `actual: "${string}" expected to not match: "${regexp}"`;
    } else {
      message = `${message}`;
    }
    throw new AssertionError({
      message,
      actual: string,
      expected: regexp,
      operator: 'doesNotMatch',
    });
  }
}

export function strict(actual: unknown, message?: string | Error): asserts actual {
  if (arguments.length === 0) {
    throw new AssertionError({
      message: "No value argument passed to `assert.ok()`",
    });
  }
  assert(actual, message);
}

export function rejects(
  asyncFn: Promise<any> | (() => Promise<any>),
  error?: RegExp | Function | Error,
): Promise<void>;

export function rejects(
  asyncFn: Promise<any> | (() => Promise<any>),
  message?: string,
): Promise<void>;

export function rejects(
  asyncFn: Promise<any> | (() => Promise<any>),
  error?: RegExp | Function | Error | string,
  message?: string,
) {
  let promise: Promise<void>;
  if (typeof asyncFn === "function") {
    try {
      promise = asyncFn();
    } catch (err) {
      return Promise.reject(err);
    }

    if (!isValidThenable(promise)) {
      return Promise.reject(
        new ERR_INVALID_RETURN_VALUE(
          "instance of Promise",
          "promiseFn",
          promise,
        ),
      );
    }
  } else if (!isValidThenable(asyncFn)) {
    return Promise.reject(
      new ERR_INVALID_ARG_TYPE("promiseFn", ["function", "Promise"], asyncFn),
    );
  } else {
    promise = asyncFn;
  }

  function onFulfilled() {
    let message = "Missing expected rejection";
    if (typeof error === "string") {
      message += `: ${error}`;
    } else if (typeof error === "function" && error.prototype !== undefined) {
      message += ` (${error.name}).`;
    } else {
      message += ".";
    }
    return Promise.reject(createAssertionError({
      message,
      operator: "rejects",
      generatedMessage: true,
    }));
  }

  function rejects_onRejected(e: Error) {
    if (
      validateThrownError(e, error, message, {
        operator: rejects,
        validationFunctionName: "validate",
      })
    ) {
      return;
    }
  }

  return promise.then(onFulfilled, rejects_onRejected);
}

export function doesNotReject(
  asyncFn: Promise<any> | (() => Promise<any>),
  error?: RegExp | Function,
): Promise<void>;

export function doesNotReject(
  asyncFn: Promise<any> | (() => Promise<any>),
  message?: string,
): Promise<void>;

export function doesNotReject(
  asyncFn: Promise<any> | (() => Promise<any>),
  error?: RegExp | Function | string,
  message?: string,
) {
  let promise: Promise<any>;
  if (typeof asyncFn === "function") {
    try {
      const value = asyncFn();
      if (!isValidThenable(value)) {
        return Promise.reject(
          new ERR_INVALID_RETURN_VALUE(
            "instance of Promise",
            "promiseFn",
            value,
          ),
        );
      }
      promise = value;
    } catch (e) {
      return Promise.reject(e);
    }
  } else if (!isValidThenable(asyncFn)) {
    return Promise.reject(
      new ERR_INVALID_ARG_TYPE("promiseFn", ["function", "Promise"], asyncFn),
    );
  } else {
    promise = asyncFn;
  }

  return promise.then(
    () => {},
    (e) => gotUnwantedException(e, error, message, doesNotReject),
  );
}

function gotUnwantedException(
  e: any,
  expected: RegExp | Function | string | null | undefined,
  message: string | Error | null | undefined,
  operator: Function,
): never {
  if (typeof expected === "string") {
    // The use case of doesNotThrow(fn, message);
    throw new AssertionError({
      message:
        `Got unwanted exception: ${expected}\nActual message: "${e.message}"`,
      operator: operator.name,
    });
  } else if (
    typeof expected === "function" && expected.prototype !== undefined
  ) {
    // The use case of doesNotThrow(fn, Error, message);
    if (e instanceof expected) {
      let msg = `Got unwanted exception: ${e.constructor?.name}`;
      if (message) {
        msg += ` ${String(message)}`;
      }
      throw new AssertionError({
        message: msg,
        operator: operator.name,
      });
    } else if (expected.prototype instanceof Error) {
      throw e;
    } else {
      const result = expected(e);
      if (result === true) {
        let msg = `Got unwanted rejection.\nActual message: "${e.message}"`;
        if (message) {
          msg += ` ${String(message)}`;
        }
        throw new AssertionError({
          message: msg,
          operator: operator.name,
        });
      }
    }
    throw e;
  } else {
    if (message) {
      throw new AssertionError({
        message: `Got unwanted exception: ${message}\nActual message: "${
          e ? e.message : String(e)
        }"`,
        operator: operator.name,
      });
    }
    throw new AssertionError({
      message: `Got unwanted exception.\nActual message: "${
        e ? e.message : String(e)
      }"`,
      operator: operator.name,
    });
  }
}

export function ifError(err: any) {
  if (err !== null && err !== undefined) {
    let message = "ifError got unwanted exception: ";

    if (typeof err === "object" && typeof err.message === "string") {
      if (err.message.length === 0 && err.constructor) {
        message += err.constructor.name;
      } else {
        message += err.message;
      }
    } else {
      // TODO(soon): Implement inspect
      // message += inspect(err);
      message += `${err}`;
    }

    const newErr = new AssertionError({
      actual: err,
      expected: null,
      operator: "ifError",
      message,
      stackStartFn: ifError,
    });

    // Make sure we actually have a stack trace!
    const origStack = err.stack;

    if (typeof origStack === "string") {
      const tmp2 = origStack.split("\n");
      tmp2.shift();
      let tmp1 = newErr!.stack?.split("\n");

      for (const errFrame of tmp2) {
        const pos = tmp1?.indexOf(errFrame);

        if (pos !== -1) {
          tmp1 = tmp1?.slice(0, pos);

          break;
        }
      }

      newErr.stack = `${tmp1?.join("\n")}\n${tmp2.join("\n")}`;
    }

    throw newErr;
  }
}

interface ValidateThrownErrorOptions {
  operator: Function;
  validationFunctionName?: string;
}

function validateThrownError(
  e: any,
  error: RegExp | Function | Error | string | null | undefined,
  message: string | undefined | null,
  options: ValidateThrownErrorOptions,
): boolean {
  if (typeof error === "string") {
    if (message != null) {
      throw new ERR_INVALID_ARG_TYPE(
        "error",
        ["Object", "Error", "Function", "RegExp"],
        error,
      );
    } else if (typeof e === "object" && e !== null) {
      if (e.message === error) {
        throw new ERR_AMBIGUOUS_ARGUMENT(
          "error/message",
          `The error message "${e.message}" is identical to the message.`,
        );
      }
    } else if (e === error) {
      throw new ERR_AMBIGUOUS_ARGUMENT(
        "error/message",
        `The error "${e}" is identical to the message.`,
      );
    }
    message = error;
    error = undefined;
  }
  if (
    error instanceof Function && error.prototype !== undefined &&
    error.prototype instanceof Error
  ) {
    // error is a constructor
    if (e instanceof error) {
      return true;
    }
    throw createAssertionError({
      message:
        `The error is expected to be an instance of "${error.name}". Received "${e?.constructor?.name}"\n\nError message:\n\n${e?.message}`,
      actual: e,
      expected: error,
      operator: options.operator.name,
      generatedMessage: true,
    });
  }
  if (error instanceof Function) {
    const received = error(e);
    if (received === true) {
      return true;
    }
    throw createAssertionError({
      message: `The ${
        options.validationFunctionName
          ? `"${options.validationFunctionName}" validation`
          : "validation"
      } function is expected to return "true". Received ${
        // inspect(received)
        // TODO(soon): Implement inspect
        received
      }\n\nCaught error:\n\n${e}`,
      actual: e,
      expected: error,
      operator: options.operator.name,
      generatedMessage: true,
    });
  }
  if (error instanceof RegExp) {
    if (error.test(String(e))) {
      return true;
    }
    throw createAssertionError({
      message:
        `The input did not match the regular expression ${error.toString()}. Input:\n\n'${
          String(e)
        }'\n`,
      actual: e,
      expected: error,
      operator: options.operator.name,
      generatedMessage: true,
    });
  }
  if (typeof error === "object" && error !== null) {
    const keys = Object.keys(error);
    if (error instanceof Error) {
      keys.push("name", "message");
    }
    for (const k of keys) {
      if (e == null) {
        throw createAssertionError({
          message: message || "object is expected to thrown, but got null",
          actual: e,
          expected: error,
          operator: options.operator.name,
          generatedMessage: message == null,
        });
      }

      if (typeof e === "string") {
        throw createAssertionError({
          message: message ||
            `object is expected to thrown, but got string: ${e}`,
          actual: e,
          expected: error,
          operator: options.operator.name,
          generatedMessage: message == null,
        });
      }
      if (typeof e === "number") {
        throw createAssertionError({
          message: message ||
            `object is expected to thrown, but got number: ${e}`,
          actual: e,
          expected: error,
          operator: options.operator.name,
          generatedMessage: message == null,
        });
      }
      if (!(k in e)) {
        throw createAssertionError({
          message: message || `A key in the expected object is missing: ${k}`,
          actual: e,
          expected: error,
          operator: options.operator.name,
          generatedMessage: message == null,
        });
      }
      const actual = e[k];

      const expected = (error as any)[k];
      if (typeof actual === "string" && expected instanceof RegExp) {
        match(actual, expected);
      } else {
        deepStrictEqual(actual, expected);
      }
    }
    return true;
  }
  if (typeof error === "undefined") {
    return true;
  }
  throw createAssertionError({
    message: `Invalid expectation: ${error}`,
    operator: options.operator.name,
    generatedMessage: true,
  });
}

function isValidThenable(maybeThennable: any): boolean {
  if (!maybeThennable) {
    return false;
  }

  if (maybeThennable instanceof Promise) {
    return true;
  }

  const isThenable = typeof maybeThennable.then === "function" &&
    typeof maybeThennable.catch === "function";

  return isThenable && typeof maybeThennable !== "function";
}

export { AssertionError };

Object.assign(strict, {
  AssertionError,
  deepEqual: deepStrictEqual,
  deepStrictEqual,
  doesNotMatch,
  doesNotReject,
  doesNotThrow,
  equal: strictEqual,
  fail,
  ifError,
  match,
  notDeepEqual: notDeepStrictEqual,
  notDeepStrictEqual,
  notEqual: notStrictEqual,
  notStrictEqual,
  ok,
  rejects,
  strict,
  strictEqual,
  throws,
});

export default Object.assign(assert, {
  AssertionError,
  deepEqual,
  deepStrictEqual,
  doesNotMatch,
  doesNotReject,
  doesNotThrow,
  equal,
  fail,
  ifError,
  match,
  notDeepEqual,
  notDeepStrictEqual,
  notEqual,
  notStrictEqual,
  ok,
  rejects,
  strict,
  strictEqual,
  throws,
});
