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

import { default as bufferUtil } from 'node-internal:buffer';
import type { Encoding } from 'node-internal:buffer';
import { validateFunction, validateString } from 'node-internal:validators';
import {
  ERR_FALSY_VALUE_REJECTION,
  type Falsy,
} from 'node-internal:internal_errors';

const { UTF8, UTF16LE, HEX, ASCII, BASE64, BASE64URL, LATIN1 } = bufferUtil;

export function normalizeEncoding(enc?: string): Encoding | undefined {
  if (
    enc == null ||
    enc === 'utf8' ||
    enc === 'utf-8' ||
    enc === 'UTF8' ||
    enc === 'UTF-8'
  )
    return UTF8;
  return getEncodingOps(enc);
}

export function getEncodingOps(enc: unknown): Encoding | undefined {
  // eslint-disable-next-line @typescript-eslint/restrict-plus-operands
  enc += '';
  // @ts-expect-error TS18046 TS complains about unknown can not have length.
  switch (enc.length) {
    case 4:
      if (enc === 'UTF8') return UTF8;
      if (enc === 'ucs2' || enc === 'UCS2') return UTF16LE;
      enc = `${enc}`.toLowerCase();
      if (enc === 'utf8') return UTF8;
      if (enc === 'ucs2') return UTF16LE;
      break;
    case 3:
      if (enc === 'hex' || enc === 'HEX' || `${enc}`.toLowerCase() === 'hex') {
        return HEX;
      }
      break;
    case 5:
      if (enc === 'ascii') return ASCII;
      if (enc === 'ucs-2') return UTF16LE;
      if (enc === 'UTF-8') return UTF8;
      if (enc === 'ASCII') return ASCII;
      if (enc === 'UCS-2') return UTF16LE;
      enc = `${enc}`.toLowerCase();
      if (enc === 'utf-8') return UTF8;
      if (enc === 'ascii') return ASCII;
      if (enc === 'ucs-2') return UTF16LE;
      break;
    case 6:
      if (enc === 'base64') return BASE64;
      if (enc === 'latin1' || enc === 'binary') return LATIN1;
      if (enc === 'BASE64') return BASE64;
      if (enc === 'LATIN1' || enc === 'BINARY') return LATIN1;
      enc = `${enc}`.toLowerCase();
      if (enc === 'base64') return BASE64;
      if (enc === 'latin1' || enc === 'binary') return LATIN1;
      break;
    case 7:
      if (
        enc === 'utf16le' ||
        enc === 'UTF16LE' ||
        `${enc}`.toLowerCase() === 'utf16le'
      ) {
        return UTF16LE;
      }
      break;
    case 8:
      if (
        enc === 'utf-16le' ||
        enc === 'UTF-16LE' ||
        `${enc}`.toLowerCase() === 'utf-16le'
      ) {
        return UTF16LE;
      }
      break;
    case 9:
      if (
        enc === 'base64url' ||
        enc === 'BASE64URL' ||
        `${enc}`.toLowerCase() === 'base64url'
      ) {
        return BASE64URL;
      }
      break;
    default:
      if (enc === '') return UTF8;
  }
  return undefined;
}

export function spliceOne(list: unknown[], index: number): void {
  for (; index + 1 < list.length; index++) list[index] = list[index + 1];
  list.pop();
}

export const ALL_PROPERTIES = 0;
export const ONLY_WRITABLE = 1;
export const ONLY_ENUMERABLE = 2;
export const ONLY_CONFIGURABLE = 4;
export const ONLY_ENUM_WRITABLE = 6;
export const SKIP_STRINGS = 8;
export const SKIP_SYMBOLS = 16;

const isNumericLookup: Record<string, boolean> = {};
export function isArrayIndex(value: unknown): value is number | string {
  switch (typeof value) {
    case 'number':
      return value >= 0 && (value | 0) === value;
    case 'string': {
      const result = isNumericLookup[value];
      if (result !== void 0) {
        return result;
      }
      const length = value.length;
      if (length === 0) {
        return (isNumericLookup[value] = false);
      }
      let ch = 0;
      let i = 0;
      for (; i < length; ++i) {
        ch = value.charCodeAt(i);
        if (
          (i === 0 && ch === 0x30 && length > 1) /* must not start with 0 */ ||
          ch < 0x30 /* 0 */ ||
          ch > 0x39 /* 9 */
        ) {
          return (isNumericLookup[value] = false);
        }
      }
      return (isNumericLookup[value] = true);
    }
    default:
      return false;
  }
}

export function getOwnNonIndexProperties(
  // deno-lint-ignore ban-types
  obj: object,
  filter: number
): (string | symbol)[] {
  let allProperties = [
    ...Object.getOwnPropertyNames(obj),
    ...Object.getOwnPropertySymbols(obj),
  ];

  if (Array.isArray(obj)) {
    allProperties = allProperties.filter((k) => !isArrayIndex(k));
  }

  if (filter === ALL_PROPERTIES) {
    return allProperties;
  }

  const result: (string | symbol)[] = [];
  for (const key of allProperties) {
    const desc = Object.getOwnPropertyDescriptor(obj, key);
    if (desc === undefined) {
      continue;
    }
    if (filter & ONLY_WRITABLE && !desc.writable) {
      continue;
    }
    if (filter & ONLY_ENUMERABLE && !desc.enumerable) {
      continue;
    }
    if (filter & ONLY_CONFIGURABLE && !desc.configurable) {
      continue;
    }
    if (filter & SKIP_STRINGS && typeof key === 'string') {
      continue;
    }
    if (filter & SKIP_SYMBOLS && typeof key === 'symbol') {
      continue;
    }
    result.push(key);
  }
  return result;
}

function callbackifyOnRejected(
  reason: unknown,
  cb: (error?: unknown) => void
): void {
  // `!reason` guard inspired by bluebird (https://github.com/petkaantonov/bluebird/blob/2207fae3572f03b089bc92d3a6cefdd278cff7ab/src/nodeify.js#L30-L43).
  // Because `null` is a special error value in callbacks which means "no error
  // occurred", we error-wrap so the callback consumer can distinguish between
  // "the promise rejected with null" or "the promise fulfilled with undefined".
  if (!reason) {
    reason = new ERR_FALSY_VALUE_REJECTION(reason as Falsy);
  }
  cb(reason);
}

// Types are taken from @types/node package
// https://github.com/DefinitelyTyped/DefinitelyTyped/blob/dccb0e78c4d3265ae06985789156451bd73312c0/types/node/util.d.ts#L1054
export function callbackify(
  fn: () => Promise<void>
): (callback: (err: Error) => void) => void;
export function callbackify<TResult>(
  fn: () => Promise<TResult>
): (callback: (err: Error, result: TResult) => void) => void;
export function callbackify<T1>(
  fn: (arg1: T1) => Promise<void>
): (arg1: T1, callback: (err: Error) => void) => void;
export function callbackify<T1, TResult>(
  fn: (arg1: T1) => Promise<TResult>
): (arg1: T1, callback: (err: Error, result: TResult) => void) => void;
export function callbackify<T1, T2>(
  fn: (arg1: T1, arg2: T2) => Promise<void>
): (arg1: T1, arg2: T2, callback: (err: Error) => void) => void;
export function callbackify<T1, T2, TResult>(
  fn: (arg1: T1, arg2: T2) => Promise<TResult>
): (
  arg1: T1,
  arg2: T2,
  callback: (err: Error | null, result: TResult) => void
) => void;
export function callbackify<T1, T2, T3>(
  fn: (arg1: T1, arg2: T2, arg3: T3) => Promise<void>
): (arg1: T1, arg2: T2, arg3: T3, callback: (err: Error) => void) => void;
export function callbackify<T1, T2, T3, TResult>(
  fn: (arg1: T1, arg2: T2, arg3: T3) => Promise<TResult>
): (
  arg1: T1,
  arg2: T2,
  arg3: T3,
  callback: (err: Error | null, result: TResult) => void
) => void;
export function callbackify<T1, T2, T3, T4>(
  fn: (arg1: T1, arg2: T2, arg3: T3, arg4: T4) => Promise<void>
): (
  arg1: T1,
  arg2: T2,
  arg3: T3,
  arg4: T4,
  callback: (err: Error) => void
) => void;
export function callbackify<T1, T2, T3, T4, TResult>(
  fn: (arg1: T1, arg2: T2, arg3: T3, arg4: T4) => Promise<TResult>
): (
  arg1: T1,
  arg2: T2,
  arg3: T3,
  arg4: T4,
  callback: (err: Error | null, result: TResult) => void
) => void;
export function callbackify<T1, T2, T3, T4, T5>(
  fn: (arg1: T1, arg2: T2, arg3: T3, arg4: T4, arg5: T5) => Promise<void>
): (
  arg1: T1,
  arg2: T2,
  arg3: T3,
  arg4: T4,
  arg5: T5,
  callback: (err: Error) => void
) => void;
export function callbackify<T1, T2, T3, T4, T5, TResult>(
  fn: (arg1: T1, arg2: T2, arg3: T3, arg4: T4, arg5: T5) => Promise<TResult>
): (
  arg1: T1,
  arg2: T2,
  arg3: T3,
  arg4: T4,
  arg5: T5,
  callback: (err: Error | null, result: TResult) => void
) => void;
export function callbackify<T1, T2, T3, T4, T5, T6>(
  fn: (
    arg1: T1,
    arg2: T2,
    arg3: T3,
    arg4: T4,
    arg5: T5,
    arg6: T6
  ) => Promise<void>
): (
  arg1: T1,
  arg2: T2,
  arg3: T3,
  arg4: T4,
  arg5: T5,
  arg6: T6,
  callback: (err: Error) => void
) => void;
export function callbackify<T1, T2, T3, T4, T5, T6, TResult>(
  fn: (
    arg1: T1,
    arg2: T2,
    arg3: T3,
    arg4: T4,
    arg5: T5,
    arg6: T6
  ) => Promise<TResult>
): (
  arg1: T1,
  arg2: T2,
  arg3: T3,
  arg4: T4,
  arg5: T5,
  arg6: T6,
  callback: (err: Error | null, result: TResult) => void
) => void;
export function callbackify<T extends (...args: unknown[]) => Promise<unknown>>(
  original: T
): T extends (...args: infer TArgs) => Promise<infer TReturn>
  ? (...params: [...TArgs, (err: Error, ret: TReturn) => unknown]) => void
  : never {
  validateFunction(original, 'original');

  function callbackified(
    this: unknown,
    ...args: [...unknown[], (err: unknown, ret: unknown) => void]
  ): void {
    const maybeCb = args.pop();
    validateFunction(maybeCb, 'last argument');
    const cb = maybeCb.bind(this);
    Reflect.apply(original, this, args).then(
      (ret: unknown) => {
        queueMicrotask(() => cb(null, ret));
      },
      (rej: unknown) => {
        queueMicrotask(() => {
          callbackifyOnRejected(rej, cb);
        });
      }
    );
  }

  const descriptors = Object.getOwnPropertyDescriptors(original);
  if (typeof descriptors.length?.value === 'number') {
    descriptors.length.value++;
  }
  if (typeof descriptors.name?.value === 'string') {
    descriptors.name.value += 'Callbackified';
  }
  const propertiesValues = Object.values(descriptors);
  for (let i = 0; i < propertiesValues.length; i++) {
    Object.setPrototypeOf(propertiesValues[i], null);
  }
  Object.defineProperties(callbackified, descriptors);
  // eslint-disable-next-line @typescript-eslint/ban-ts-comment
  // @ts-expect-error
  return callbackified;
}

export function parseEnv(content: string): Record<string, string> {
  validateString(content, 'content');

  const result: Record<string, string> = {};
  const lines = content.split('\n');

  for (let i = 0; i < lines.length; i++) {
    let line = lines[i];
    if (line === undefined) continue;

    if (!line.trim()) continue;
    if (line.trimStart().startsWith('#')) continue;
    if (line.trimStart().startsWith('export '))
      line = line.substring(line.indexOf('export ') + 7);

    const equalIndex = line.indexOf('=');
    if (equalIndex === -1) continue;

    const key = line.substring(0, equalIndex).trim();
    if (!key) continue;

    let value = line.substring(equalIndex + 1).trimStart();
    if (value.length > 0) {
      const maybeQuote = value[0];
      if (maybeQuote === '"' || maybeQuote === "'" || maybeQuote === '`') {
        // Check if the closing quote is on the same line
        const closeIndex = value.indexOf(maybeQuote, 1);

        if (closeIndex !== -1) {
          // Found closing quote on same line
          value = value.substring(1, closeIndex);
          // Only handle escape sequences for double quotes
          if (maybeQuote === '"') value = value.replace(/\\n/g, '\n');
          // For single quotes and backticks, keep \n as literal
        } else {
          // Check for multiline strings
          let fullValue = value.substring(1); // Remove opening quote
          let currentLine = i;
          let foundClosingQuote = false;

          // Look for closing quote in subsequent lines
          while (currentLine < lines.length - 1) {
            currentLine++;
            const nextLine = lines[currentLine];
            if (nextLine !== undefined) {
              const closeInNextLine = nextLine.indexOf(maybeQuote);
              if (closeInNextLine !== -1) {
                // Found closing quote
                fullValue += '\n' + nextLine.substring(0, closeInNextLine);
                value = fullValue;

                // Only handle escape sequences for double quotes
                if (maybeQuote === '"') {
                  value = value.replace(/\\n/g, '\n');
                }

                foundClosingQuote = true;
                i = currentLine; // Update line counter
                break;
              } else {
                // Continue building multiline value
                fullValue += '\n' + nextLine;
              }
            }
          }

          if (!foundClosingQuote) {
            if (value.length === 1) {
              // Just the quote character, return it as the value
              value = maybeQuote;
            } else {
              // Return content after the opening quote
              value = value.substring(1);
            }
          }
        }
      } else {
        const hashIndex = value.indexOf('#');
        if (hashIndex !== -1) value = value.substring(0, hashIndex);
        value = value.trimEnd();
      }
    }
    result[key] = value;
  }
  return result;
}

export type NonEmptyArray<T> = [T, ...T[]];
export type PositiveInteger<T extends number> = T extends 0
  ? never
  : `${T}` extends `${infer _}.${infer _}`
    ? never
    : `${T}` extends `-${infer _}`
      ? never
      : T;
export type FixedLengthArray<
  T,
  Length extends number,
  Accumulator extends NonEmptyArray<T> = [T],
> =
  Length extends PositiveInteger<Length>
    ? number extends Length
      ? NonEmptyArray<T>
      : Length extends Accumulator['length']
        ? Accumulator
        : FixedLengthArray<T, Length, [T, ...Accumulator]>
    : never;
