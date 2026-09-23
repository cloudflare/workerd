// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Type definitions for the `node-internal:buffer_native` module
// (src/workerd/api/node/buffer-native.h). Each function wraps exactly one V8,
// libc, simdutf, nbytes, kj, or i18n call made by src/workerd/api/node/buffer.c++.

// jsg::JsString::WriteFlags
export const WRITE_NONE: number;
export const WRITE_NULL_TERMINATION: number;
export const WRITE_REPLACE_INVALID_UTF8: number;

// V8 strings
export function utf8Length(string: string): number;
export function writeOneByte(
  string: string,
  dest: Uint8Array,
  flags: number
): number;
export function writeUtf8(
  string: string,
  dest: Uint8Array,
  flags: number
): number;
export function writeUtf16(
  string: string,
  dest: Uint8Array,
  flags: number
): number;
export function newFromOneByte(bytes: Uint8Array): string;
export function newFromUtf8(bytes: Uint8Array): string;
export function newFromTwoByte(bytes: Uint8Array): string;

// libc
export function memcmp(
  one: Uint8Array,
  two: Uint8Array,
  length: number
): number;

// simdutf
export function simdutfMaximalBinaryLengthFromBase64(input: Uint8Array): number;
export function simdutfBase64ToBinary(
  input: Uint8Array,
  output: Uint8Array
): number;
export function simdutfBase64LengthFromBinary(length: number): number;
export function simdutfBase64UrlLengthFromBinary(length: number): number;
export function simdutfBinaryToBase64(
  input: Uint8Array,
  output: Uint8Array
): number;
export function simdutfBinaryToBase64Url(
  input: Uint8Array,
  output: Uint8Array
): number;
export function simdutfValidateAscii(input: Uint8Array): boolean;
export function simdutfValidateUtf8(input: Uint8Array): boolean;

// nbytes
export function nbytesBase64Decode(
  output: Uint8Array,
  input: Uint8Array
): number;
export function nbytesSwapBytes16(data: Uint8Array): boolean;
export function nbytesSwapBytes32(data: Uint8Array): boolean;
export function nbytesSwapBytes64(data: Uint8Array): boolean;

// kj
export function kjEncodeHex(input: Uint8Array): Uint8Array;

// workerd::api::node::i18n
export function transcode(
  source: Uint8Array,
  fromEncoding: number,
  toEncoding: number
): Uint8Array;
