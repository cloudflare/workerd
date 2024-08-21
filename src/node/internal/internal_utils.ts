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
import { default as bufferUtil } from 'node-internal:buffer';
import type { Encoding } from 'node-internal:buffer';

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
  return slowCases(enc);
}

export function slowCases(enc: string): Encoding | undefined {
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

export function spliceOne(list: (string | undefined)[], index: number) {
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

export function createDeferredPromise() {
  let resolve;
  let reject;

  // eslint-disable-next-line promise/param-names
  const promise = new Promise((res, rej) => {
    resolve = res;
    reject = rej;
  });
  return {
    promise,
    resolve,
    reject,
  };
}
