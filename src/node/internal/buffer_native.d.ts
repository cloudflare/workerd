// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Type definitions for the `node-internal:buffer_native` module
// (src/rust/api/buffer.rs), the primitives that stand in for the V8, libc,
// simdutf, nbytes, kj, i18n, and string search calls of
// src/workerd/api/node/buffer.c++. Functions that take `start` and `end`
// operate on that range of the array.

// v8::String::WriteFlags
export const WRITE_NONE: number;
export const WRITE_REPLACE_INVALID_UTF8: number;

// V8 strings
export function utf8Length(string: string): number;
export function writeOneByte(
  string: string,
  dest: Uint8Array,
  start: number,
  end: number,
  flags: number
): number;
export function writeUtf8(
  string: string,
  dest: Uint8Array,
  start: number,
  end: number,
  flags: number
): number;
export function writeUtf16(
  string: string,
  dest: Uint8Array,
  start: number,
  end: number,
  flags: number
): number;
export function newFromOneByte(
  bytes: Uint8Array,
  start: number,
  end: number
): string;
export function newFromUtf8(
  bytes: Uint8Array,
  start: number,
  end: number
): string;
export function newFromTwoByte(
  bytes: Uint8Array,
  start: number,
  end: number
): string;

// String search
export function searchOneByte(
  haystack: Uint8Array,
  needle: Uint8Array,
  startIndex: number,
  isForward: boolean
): number;
export function searchTwoByte(
  haystack: Uint8Array,
  haystackLength: number,
  needle: Uint8Array,
  needleLength: number,
  startIndex: number,
  isForward: boolean
): number;

// Comparison
export function memcmp(
  one: Uint8Array,
  oneStart: number,
  two: Uint8Array,
  twoStart: number,
  length: number
): number;

// Base64
export function decodeBase64(string: string): Uint8Array;
export function encodeBase64(
  bytes: Uint8Array,
  start: number,
  end: number
): string;
export function encodeBase64Url(
  bytes: Uint8Array,
  start: number,
  end: number
): string;
export function decodeBase64Into(
  output: Uint8Array,
  start: number,
  end: number,
  input: Uint8Array
): number;

// Hex
export function encodeHex(
  bytes: Uint8Array,
  start: number,
  end: number
): string;

// Validation
export function validateAscii(input: Uint8Array): boolean;
export function validateUtf8(input: Uint8Array): boolean;

// Byte order
export function swapBytes16(data: Uint8Array): boolean;
export function swapBytes32(data: Uint8Array): boolean;
export function swapBytes64(data: Uint8Array): boolean;

// Transcoding
export function transcode(
  source: Uint8Array,
  fromEncoding: number,
  toEncoding: number
): Uint8Array;
