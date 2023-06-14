// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Adapted from Deno and Node.js:
// Copyright 2018-2023 the Deno authors. All rights reserved. MIT license.
// Adapted from Node.js. Copyright Joyent, Inc. and other Node contributors.
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

/* todo: the following is adopted code, enabling linting one day */
/* eslint-disable */

import { ERR_INVALID_ARG_TYPE } from 'node-internal:internal_errors';
import util from 'node-internal:inspect_polyfill';

let blue = "";
let green = "";
let red = "";
let defaultColor = "";

const kReadableOperator: { [key: string]: string } = {
  deepStrictEqual: "Expected values to be strictly deep-equal:",
  strictEqual: "Expected values to be strictly equal:",
  strictEqualObject: 'Expected "actual" to be reference-equal to "expected":',
  deepEqual: "Expected values to be loosely deep-equal:",
  notDeepStrictEqual: 'Expected "actual" not to be strictly deep-equal to:',
  notStrictEqual: 'Expected "actual" to be strictly unequal to:',
  notStrictEqualObject:
    'Expected "actual" not to be reference-equal to "expected":',
  notDeepEqual: 'Expected "actual" not to be loosely deep-equal to:',
  notIdentical: "Values have same structure but are not reference-equal:",
  notDeepEqualUnequal: "Expected values not to be loosely deep-equal:",
};

// Comparing short primitives should just show === / !== instead of using the
// diff.
const kMaxShortLength = 12;

export function copyError(source: any): Error {
  const keys = Object.keys(source);
  const target = Object.create(Object.getPrototypeOf(source));
  for (const key of keys) {
    const desc = Object.getOwnPropertyDescriptor(source, key);

    if (desc !== undefined) {
      Object.defineProperty(target, key, desc);
    }
  }
  Object.defineProperty(target, "message", { value: source.message });
  return target;
}

export function inspectValue(val: unknown): string {
  return util.inspect(
    val,
    {
      compact: true,
      customInspect: false,
      depth: 1000,
      maxArrayLength: Infinity,
      // Assert compares only enumerable properties (with a few exceptions).
      showHidden: false,
      // Assert does not detect proxies currently.
      showProxy: false,
      sorted: true,
      // Inspect getters as we also check them when comparing entries.
      getters: true,
    },
  );
}

export function createErrDiff(
  actual: unknown,
  expected: unknown,
  operator: string,
): string {
  let other = "";
  let res = "";
  let end = "";
  let skipped = false;
  const actualInspected = inspectValue(actual);
  const actualLines = actualInspected.split("\n");
  const expectedLines = inspectValue(expected).split("\n");

  let i = 0;
  let indicator = "";

  // In case both values are objects or functions explicitly mark them as not
  // reference equal for the `strictEqual` operator.
  if (
    operator === "strictEqual" &&
    ((typeof actual === "object" && actual !== null &&
      typeof expected === "object" && expected !== null) ||
      (typeof actual === "function" && typeof expected === "function"))
  ) {
    operator = "strictEqualObject";
  }

  // If "actual" and "expected" fit on a single line and they are not strictly
  // equal, check further special handling.
  if (
    actualLines.length === 1 && expectedLines.length === 1 &&
    actualLines[0] !== expectedLines[0]
  ) {
    const actualRaw = actualLines[0];
    const expectedRaw = expectedLines[0];
    const inputLength = (actualRaw as string).length + (expectedRaw as string).length;
    // If the character length of "actual" and "expected" together is less than
    // kMaxShortLength and if neither is an object and at least one of them is
    // not `zero`, use the strict equal comparison to visualize the output.
    if (inputLength <= kMaxShortLength) {
      if (
        (typeof actual !== "object" || actual === null) &&
        (typeof expected !== "object" || expected === null) &&
        (actual !== 0 || expected !== 0)
      ) { // -0 === +0
        return `${kReadableOperator[operator]}\n\n` +
          `${actualLines[0]} !== ${expectedLines[0]}\n`;
      }
    } else if (operator !== "strictEqualObject") {
      // If the stderr is a tty and the input length is lower than the current
      // columns per line, add a mismatch indicator below the output. If it is
      // not a tty, use a default value of 80 characters.
      const maxLength = 80;
      if (inputLength < maxLength) {
        while ((actualRaw as string)[i] === (expectedRaw as string)[i]) {
          i++;
        }
        // Ignore the first characters.
        if (i > 2) {
          // Add position indicator for the first mismatch in case it is a
          // single line and the input length is less than the column length.
          indicator = `\n  ${" ".repeat(i)}^`;
          i = 0;
        }
      }
    }
  }

  // Remove all ending lines that match (this optimizes the output for
  // readability by reducing the number of total changed lines).
  let a = actualLines[actualLines.length - 1];
  let b = expectedLines[expectedLines.length - 1];
  while (a === b) {
    if (i++ < 3) {
      end = `\n  ${a}${end}`;
    } else {
      other = a as string;
    }
    actualLines.pop();
    expectedLines.pop();
    if (actualLines.length === 0 || expectedLines.length === 0) {
      break;
    }
    a = actualLines[actualLines.length - 1];
    b = expectedLines[expectedLines.length - 1];
  }

  const maxLines = Math.max(actualLines.length, expectedLines.length);
  // Strict equal with identical objects that are not identical by reference.
  // E.g., assert.deepStrictEqual({ a: Symbol() }, { a: Symbol() })
  if (maxLines === 0) {
    // We have to get the result again. The lines were all removed before.
    const actualLines = actualInspected.split("\n");

    // Only remove lines in case it makes sense to collapse those.
    if (actualLines.length > 50) {
      actualLines[46] = `${blue}...${defaultColor}`;
      while (actualLines.length > 47) {
        actualLines.pop();
      }
    }

    return `${kReadableOperator['notIdentical']}\n\n${actualLines.join("\n")}\n`;
  }

  // There were at least five identical lines at the end. Mark a couple of
  // skipped.
  if (i >= 5) {
    end = `\n${blue}...${defaultColor}${end}`;
    skipped = true;
  }
  if (other !== "") {
    end = `\n  ${other}${end}`;
    other = "";
  }

  let printedLines = 0;
  let identical = 0;
  const msg = kReadableOperator[operator] +
    `\n${green}+ actual${defaultColor} ${red}- expected${defaultColor}`;
  const skippedMsg = ` ${blue}...${defaultColor} Lines skipped`;

  let lines = actualLines;
  let plusMinus = `${green}+${defaultColor}`;
  let maxLength = expectedLines.length;
  if (actualLines.length < maxLines) {
    lines = expectedLines;
    plusMinus = `${red}-${defaultColor}`;
    maxLength = actualLines.length;
  }

  for (i = 0; i < maxLines; i++) {
    if (maxLength < i + 1) {
      // If more than two former lines are identical, print them. Collapse them
      // in case more than five lines were identical.
      if (identical > 2) {
        if (identical > 3) {
          if (identical > 4) {
            if (identical === 5) {
              res += `\n  ${lines[i - 3]}`;
              printedLines++;
            } else {
              res += `\n${blue}...${defaultColor}`;
              skipped = true;
            }
          }
          res += `\n  ${lines[i - 2]}`;
          printedLines++;
        }
        res += `\n  ${lines[i - 1]}`;
        printedLines++;
      }
      // No identical lines before.
      identical = 0;
      // Add the expected line to the cache.
      if (lines === actualLines) {
        res += `\n${plusMinus} ${lines[i]}`;
      } else {
        other += `\n${plusMinus} ${lines[i]}`;
      }
      printedLines++;
      // Only extra actual lines exist
      // Lines diverge
    } else {
      const expectedLine = expectedLines[i];
      let actualLine = actualLines[i];
      // If the lines diverge, specifically check for lines that only diverge by
      // a trailing comma. In that case it is actually identical and we should
      // mark it as such.
      let divergingLines = actualLine !== expectedLine &&
        (!(actualLine as string).endsWith(",") ||
          (actualLine as string).slice(0, -1) !== expectedLine);
      // If the expected line has a trailing comma but is otherwise identical,
      // add a comma at the end of the actual line. Otherwise the output could
      // look weird as in:
      //
      //   [
      //     1         // No comma at the end!
      // +   2
      //   ]
      //
      if (
        divergingLines &&
        (expectedLine as string).endsWith(",") &&
        (expectedLine as string).slice(0, -1) === actualLine
      ) {
        divergingLines = false;
        actualLine += ",";
      }
      if (divergingLines) {
        // If more than two former lines are identical, print them. Collapse
        // them in case more than five lines were identical.
        if (identical > 2) {
          if (identical > 3) {
            if (identical > 4) {
              if (identical === 5) {
                res += `\n  ${actualLines[i - 3]}`;
                printedLines++;
              } else {
                res += `\n${blue}...${defaultColor}`;
                skipped = true;
              }
            }
            res += `\n  ${actualLines[i - 2]}`;
            printedLines++;
          }
          res += `\n  ${actualLines[i - 1]}`;
          printedLines++;
        }
        // No identical lines before.
        identical = 0;
        // Add the actual line to the result and cache the expected diverging
        // line so consecutive diverging lines show up as +++--- and not +-+-+-.
        res += `\n${green}+${defaultColor} ${actualLine}`;
        other += `\n${red}-${defaultColor} ${expectedLine}`;
        printedLines += 2;
        // Lines are identical
      } else {
        // Add all cached information to the result before adding other things
        // and reset the cache.
        res += other;
        other = "";
        identical++;
        // The very first identical line since the last diverging line is be
        // added to the result.
        if (identical <= 2) {
          res += `\n  ${actualLine}`;
          printedLines++;
        }
      }
    }
    // Inspected object to big (Show ~50 rows max)
    if (printedLines > 50 && i < maxLines - 2) {
      return `${msg}${skippedMsg}\n${res}\n${blue}...${defaultColor}${other}\n` +
        `${blue}...${defaultColor}`;
    }
  }

  return `${msg}${skipped ? skippedMsg : ""}\n${res}${other}${end}${indicator}`;
}

export interface AssertionErrorDetailsDescriptor {
  message: string;
  actual: unknown;
  expected: unknown;
  operator: string;
  stack: Error;
}

export interface AssertionErrorConstructorOptions {
  message: string | Error | undefined;
  actual?: unknown;
  expected?: unknown;
  operator?: string;
  details?: AssertionErrorDetailsDescriptor[];
  // deno-lint-ignore ban-types
  stackStartFn?: Function;
  // Compatibility with older versions.
  // deno-lint-ignore ban-types
  stackStartFunction?: Function;
}

interface ErrorWithStackTraceLimit extends ErrorConstructor {
  stackTraceLimit: number;
}

export class AssertionError extends Error {
  [key: string]: unknown;

  // deno-lint-ignore constructor-super
  constructor(options: AssertionErrorConstructorOptions) {
    if (typeof options !== "object" || options === null) {
      throw new ERR_INVALID_ARG_TYPE("options", "Object", options);
    }
    const {
      message,
      operator,
      stackStartFn,
      details,
      // Compatibility with older versions.
      stackStartFunction,
    } = options;
    let {
      actual,
      expected,
    } = options;

    // TODO(schwarzkopfb): `stackTraceLimit` should be added to `ErrorConstructor` in
    // cli/dts/lib.deno.shared_globals.d.ts
    const limit = (Error as ErrorWithStackTraceLimit).stackTraceLimit;
    (Error as ErrorWithStackTraceLimit).stackTraceLimit = 0;

    if (message != null) {
      super(String(message));
    } else {
      // Prevent the error stack from being visible by duplicating the error
      // in a very close way to the original in case both sides are actually
      // instances of Error.
      if (
        typeof actual === "object" && actual !== null &&
        typeof expected === "object" && expected !== null &&
        "stack" in actual && actual instanceof Error &&
        "stack" in expected && expected instanceof Error
      ) {
        actual = copyError(actual);
        expected = copyError(expected);
      }

      if (operator === "deepStrictEqual" || operator === "strictEqual") {
        super(createErrDiff(actual, expected, operator));
      } else if (
        operator === "notDeepStrictEqual" ||
        operator === "notStrictEqual"
      ) {
        // In case the objects are equal but the operator requires unequal, show
        // the first object and say A equals B
        let base = kReadableOperator[operator];
        const res = inspectValue(actual).split("\n");

        // In case "actual" is an object or a function, it should not be
        // reference equal.
        if (
          operator === "notStrictEqual" &&
          ((typeof actual === "object" && actual !== null) ||
            typeof actual === "function")
        ) {
          base = kReadableOperator['notStrictEqualObject'];
        }

        // Only remove lines in case it makes sense to collapse those.
        if (res.length > 50) {
          res[46] = `${blue}...${defaultColor}`;
          while (res.length > 47) {
            res.pop();
          }
        }

        // Only print a single input.
        if (res.length === 1) {
          super(`${base}${res[0]!.length > 5 ? "\n\n" : " "}${res[0]}`);
        } else {
          super(`${base}\n\n${res.join("\n")}\n`);
        }
      } else {
        let res = inspectValue(actual);
        let other = inspectValue(expected);
        const knownOperator = kReadableOperator[operator ?? ""];
        if (operator === "notDeepEqual" && res === other) {
          res = `${knownOperator}\n\n${res}`;
          if (res.length > 1024) {
            res = `${res.slice(0, 1021)}...`;
          }
          super(res);
        } else {
          if (res.length > 512) {
            res = `${res.slice(0, 509)}...`;
          }
          if (other.length > 512) {
            other = `${other.slice(0, 509)}...`;
          }
          if (operator === "deepEqual") {
            res = `${knownOperator}\n\n${res}\n\nshould loosely deep-equal\n\n`;
          } else {
            const newOp = kReadableOperator[`${operator}Unequal`];
            if (newOp) {
              res = `${newOp}\n\n${res}\n\nshould not loosely deep-equal\n\n`;
            } else {
              other = ` ${operator} ${other}`;
            }
          }
          super(`${res}${other}`);
        }
      }
    }

    (Error as ErrorWithStackTraceLimit).stackTraceLimit = limit;

    (this as any).generatedMessage = !message;
    Object.defineProperty(this, "name", {
      __proto__: null,
      value: "AssertionError [ERR_ASSERTION]",
      enumerable: false,
      writable: true,
      configurable: true,
      // deno-lint-ignore no-explicit-any
    } as any);
    (this as any).code = "ERR_ASSERTION";

    if (details) {
      (this as any).actual = undefined;
      (this as any).expected = undefined;
      (this as any).operator = undefined;

      for (let i = 0; i < details.length; i++) {
        this["message " + i] = (details[i] as any).message;
        this["actual " + i] = (details[i] as any).actual;
        this["expected " + i] = (details[i] as any).expected;
        this["operator " + i] = (details[i] as any).operator;
        this["stack trace " + i] = (details[i] as any).stack;
      }
    } else {
      (this as any).actual = actual;
      (this as any).expected = expected;
      (this as any).operator = operator;
    }

    // @ts-ignore this function is not available in lib.dom.d.ts
    Error.captureStackTrace(this, stackStartFn || stackStartFunction);
    // Create error message including the error code in the name.
    this.stack;
    // Reset the name.
    this.name = "AssertionError";
  }

  override toString() {
    return `${this.name} [${(this as any).code}]: ${this.message}`;
  }

  // TODO(soon): Implement inspect
  // [inspect.custom](_recurseTimes: number, ctx: Record<string, unknown>) {
  //   // Long strings should not be fully inspected.
  //   const tmpActual = this.actual;
  //   const tmpExpected = this.expected;

  //   for (const name of ["actual", "expected"]) {
  //     if (typeof this[name] === "string") {
  //       const value = this[name] as string;
  //       const lines = value.split("\n");
  //       if (lines.length > 10) {
  //         lines.length = 10;
  //         this[name] = `${lines.join("\n")}\n...`;
  //       } else if (value.length > 512) {
  //         this[name] = `${value.slice(512)}...`;
  //       }
  //     }
  //   }

  //   // This limits the `actual` and `expected` property default inspection to
  //   // the minimum depth. Otherwise those values would be too verbose compared
  //   // to the actual error message which contains a combined view of these two
  //   // input values.
  //   // TODO(soon): Implement inspect
  //   // const result = inspect(this, {
  //   //   ...ctx,
  //   //   customInspect: false,
  //   //   depth: 0,
  //   // });
  //   const result = `${this}`;

  //   // Reset the properties after inspection.
  //   (this as any).actual = tmpActual;
  //   (this as any).expected = tmpExpected;

  //   return result;
  // }
}
