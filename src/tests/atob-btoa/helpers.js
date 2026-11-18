// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared by more than one behavior module.

import { ok, strictEqual, throws } from 'node:assert';

export const kInvalidBase64 =
  'atob() called with invalid base64-encoded data. (Only whitespace, ' +
  "'+', '/', alphanumeric ASCII, and up to two terminal '=' signs when " +
  'the input data length is divisible by 4 are allowed.)';

export const kNotLatin1 =
  'btoa() can only operate on characters in the Latin1 (ISO/IEC 8859-1) ' +
  'range.';

// A property descriptor's attributes as a short string: its kind ('data'
// or 'accessor'), then w, e and c for writable, enumerable and
// configurable.
export function attributesOf(object, key) {
  const descriptor = Object.getOwnPropertyDescriptor(object, key);
  if (descriptor === undefined) return 'none';
  return [
    'value' in descriptor ? 'data' : 'accessor',
    descriptor.writable ? 'w' : '',
    descriptor.enumerable ? 'e' : '',
    descriptor.configurable ? 'c' : '',
  ].join('');
}

// Asserts `fn` throws the InvalidCharacterError that atob() and btoa()
// throw, with `message`.
export function throwsInvalidCharacter(fn, message) {
  throws(fn, (error) => {
    ok(error instanceof DOMException);
    strictEqual(Object.getPrototypeOf(error), DOMException.prototype);
    strictEqual(error.name, 'InvalidCharacterError');
    strictEqual(error.code, 5);
    strictEqual(error.code, DOMException.INVALID_CHARACTER_ERR);
    strictEqual(error.message, message);
    return true;
  });
}

// A string of `length` code units below 0x100 that takes every byte value.
export function binaryString(length, seed = 7) {
  const units = new Array(length);
  for (let i = 0; i < length; i++) units[i] = (i * 31 + seed) & 0xff;
  let result = '';
  for (let i = 0; i < length; i += 4096) {
    result += String.fromCharCode(...units.slice(i, i + 4096));
  }
  return result;
}

const kAlphabet =
  'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

// RFC 4648 base64 of a string of code units below 0x100, written out
// independently of the runtime.
export function referenceEncode(input) {
  let result = '';
  for (let i = 0; i < input.length; i += 3) {
    const a = input.charCodeAt(i);
    const b = i + 1 < input.length ? input.charCodeAt(i + 1) : undefined;
    const c = i + 2 < input.length ? input.charCodeAt(i + 2) : undefined;
    result += kAlphabet[a >> 2];
    result += kAlphabet[((a & 0x03) << 4) | ((b ?? 0) >> 4)];
    result +=
      b === undefined ? '=' : kAlphabet[((b & 0x0f) << 2) | ((c ?? 0) >> 6)];
    result += c === undefined ? '=' : kAlphabet[c & 0x3f];
  }
  return result;
}

// WHATWG Infra's forgiving-base64 decode, written out step by step.
// Returns null where the algorithm returns failure.
export function referenceDecode(input) {
  let data = input.replace(/[\t\n\f\r ]/g, '');
  if (data.length % 4 === 0) data = data.replace(/={1,2}$/, '');
  if (data.length % 4 === 1) return null;
  if (!/^[A-Za-z0-9+/]*$/.test(data)) return null;
  let result = '';
  let buffer = 0;
  let bits = 0;
  for (const char of data) {
    buffer = (buffer << 6) | kAlphabet.indexOf(char);
    bits += 6;
    if (bits === 24) {
      result += String.fromCharCode(
        (buffer >> 16) & 0xff,
        (buffer >> 8) & 0xff,
        buffer & 0xff
      );
      buffer = 0;
      bits = 0;
    }
  }
  if (bits === 12) {
    result += String.fromCharCode((buffer >> 4) & 0xff);
  } else if (bits === 18) {
    result += String.fromCharCode((buffer >> 10) & 0xff, (buffer >> 2) & 0xff);
  }
  return result;
}

// A deterministic pseudo-random generator (mulberry32), so that a failure
// reproduces.
export function prng(seed) {
  let state = seed >>> 0;
  return () => {
    state = (state + 0x6d2b79f5) >>> 0;
    let t = state;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
