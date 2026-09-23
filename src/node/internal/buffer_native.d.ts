// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Type definitions for the `node-internal:buffer_native` module
// (src/workerd/api/node/buffer-native.h), which makes the V8, libc, simdutf,
// nbytes, kj, and i18n calls of src/workerd/api/node/buffer.c++. Functions
// that take `start` and `end` operate on that range of the array.

// jsg::JsString::WriteFlags
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

// libc
export function memchr(
  bytes: Uint8Array,
  start: number,
  end: number,
  value: number
): number;
export function memrchr(
  bytes: Uint8Array,
  start: number,
  end: number,
  value: number
): number;
export function memcmp(
  one: Uint8Array,
  oneStart: number,
  two: Uint8Array,
  twoStart: number,
  length: number
): number;

// simdutf
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
export function simdutfValidateAscii(input: Uint8Array): boolean;
export function simdutfValidateUtf8(input: Uint8Array): boolean;

// nbytes
export function nbytesBase64Decode(
  output: Uint8Array,
  start: number,
  end: number,
  input: Uint8Array
): number;
export function nbytesSwapBytes16(data: Uint8Array): boolean;
export function nbytesSwapBytes32(data: Uint8Array): boolean;
export function nbytesSwapBytes64(data: Uint8Array): boolean;

// kj
export function encodeHex(
  bytes: Uint8Array,
  start: number,
  end: number
): string;

// workerd::api::node::i18n
export function transcode(
  source: Uint8Array,
  fromEncoding: number,
  toEncoding: number
): Uint8Array;
