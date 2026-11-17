// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
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

// Port of Node.js lib/internal/util/diff.js and the Myers diff in
// lib/internal/assert/myers_diff.js. The output order and the meaning of the
// operation codes follow Node.js exactly: INSERT entries carry values from
// `actual` and DELETE entries carry values from `expected`.

import { ERR_OUT_OF_RANGE } from 'node-internal:internal_errors';
import { validateString, validateStringArray } from 'node-internal:validators';

const kOperations = {
  DELETE: -1,
  NOP: 0,
  INSERT: 1,
} as const;

export type DiffOperation = (typeof kOperations)[keyof typeof kOperations];
export type DiffEntry = [DiffOperation, string];

type Sequence = string | readonly string[];

function myersDiff(actual: Sequence, expected: Sequence): DiffEntry[] {
  const actualLength = actual.length;
  const expectedLength = expected.length;
  const max = actualLength + expectedLength;

  if (max > 2 ** 31 - 1) {
    throw new ERR_OUT_OF_RANGE('myersDiff input size', '< 2^31', max);
  }

  const v = new Int32Array(2 * max + 1);
  const trace: Int32Array[] = [];

  for (let diffLevel = 0; diffLevel <= max; diffLevel++) {
    trace.push(new Int32Array(v));

    for (
      let diagonalIndex = -diffLevel;
      diagonalIndex <= diffLevel;
      diagonalIndex += 2
    ) {
      const offset = diagonalIndex + max;
      const previousOffset = v[offset - 1] as number;
      const nextOffset = v[offset + 1] as number;
      let x =
        diagonalIndex === -diffLevel ||
        (diagonalIndex !== diffLevel && previousOffset < nextOffset)
          ? nextOffset
          : previousOffset + 1;
      let y = x - diagonalIndex;

      while (
        x < actualLength &&
        y < expectedLength &&
        actual[x] === expected[y]
      ) {
        x++;
        y++;
      }

      v[offset] = x;

      if (x >= actualLength && y >= expectedLength) {
        return backtrack(
          trace,
          actual,
          expected,
          actualLength,
          expectedLength,
          max
        );
      }
    }
  }

  // The loop always reaches the end of both sequences by diffLevel === max.
  return [];
}

function backtrack(
  trace: Int32Array[],
  actual: Sequence,
  expected: Sequence,
  actualLength: number,
  expectedLength: number,
  max: number
): DiffEntry[] {
  let x = actualLength;
  let y = expectedLength;
  const result: DiffEntry[] = [];

  for (let diffLevel = trace.length - 1; diffLevel >= 0; diffLevel--) {
    const v = trace[diffLevel] as Int32Array;
    const diagonalIndex = x - y;
    const offset = diagonalIndex + max;

    let prevDiagonalIndex: number;
    if (
      diagonalIndex === -diffLevel ||
      (diagonalIndex !== diffLevel &&
        (v[offset - 1] as number) < (v[offset + 1] as number))
    ) {
      prevDiagonalIndex = diagonalIndex + 1;
    } else {
      prevDiagonalIndex = diagonalIndex - 1;
    }

    const prevX = v[prevDiagonalIndex + max] as number;
    const prevY = prevX - prevDiagonalIndex;

    while (x > prevX && y > prevY) {
      result.push([kOperations.NOP, actual[x - 1] as string]);
      x--;
      y--;
    }

    if (diffLevel > 0) {
      if (x > prevX) {
        result.push([kOperations.INSERT, actual[--x] as string]);
      } else {
        result.push([kOperations.DELETE, expected[--y] as string]);
      }
    }
  }

  return result;
}

function validateInput(
  value: unknown,
  name: string
): asserts value is Sequence {
  if (Array.isArray(value)) validateStringArray(value, name);
  else validateString(value, name);
}

export function diff(actual: unknown, expected: unknown): DiffEntry[] {
  if (actual === expected) {
    return [];
  }

  validateInput(actual, 'actual');
  validateInput(expected, 'expected');

  return myersDiff(actual, expected).reverse();
}
