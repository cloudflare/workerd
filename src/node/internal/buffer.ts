// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

// The TypeScript implementation of `node-internal:buffer`, selected by the
// NODEJS_BUFFER_TS autogate. It is a port of src/workerd/api/node/buffer.c++
// (BufferUtil) and follows it function by function, so the two can be
// compared side by side.
//
// Wherever the C++ calls into V8, simdutf, nbytes, kj, or i18n, this module
// calls the same function through `node-internal:buffer_native` rather than
// reimplementing it. Everything else is ported directly.
//
// The C++ receives its arguments through JSG, which converts them to the
// declared C++ types and throws a TypeError on failure. The `unwrap*` helpers
// below reproduce those conversions and messages.

// Typed-array element reads below are always in bounds, as they are in the C++.
/* eslint-disable @typescript-eslint/no-non-null-assertion */

import { default as native } from 'node-internal:buffer_native';
import { searchString } from 'node-internal:buffer_string_search';
import { default as utilImpl } from 'node-internal:util';

// =======================================================================================
// Encoding (src/workerd/api/node/i18n.h)

export type Encoding = number;

export const ASCII: Encoding = 0;
export const LATIN1: Encoding = 1;
export const UTF8: Encoding = 2;
export const UTF16LE: Encoding = 3;
export const BASE64: Encoding = 4;
export const BASE64URL: Encoding = 5;
export const HEX: Encoding = 6;

// i18n::canBeTranscoded
function canBeTranscoded(encoding: Encoding): boolean {
  switch (encoding) {
    case ASCII:
    case LATIN1:
    case UTF16LE:
    case UTF8:
      return true;
    default:
      return false;
  }
}

// BufferUtil::NativeDecoderFields
const kIncompleteCharactersStart = 0;
const kIncompleteCharactersEnd = 4;
const kMissingBytes = 4;
const kBufferedBytes = 5;
const kEncoding = 6;
const kSize = 7;

// =======================================================================================
// Helpers standing in for JSG and KJ

// v8::ArrayBuffer::kMaxByteLength, in a build without the V8 sandbox.
const kMaxByteLength = Number.MAX_SAFE_INTEGER;

const kResourceName = 'BufferUtil';

// v8::Value::IsUint8Array
function isUint8Array(value: unknown): value is Uint8Array {
  return utilImpl.isUint8Array(value);
}

// v8::Value::ToNumber. Unlike Number(), throws for BigInts and Symbols.
function toNumber(value: unknown): number {
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-type-conversion
  return +(value as number);
}

// KJ_ASSERT / KJ_UNREACHABLE. JSG reports internal errors to JavaScript as a
// generic Error without the details.
function internalError(): Error {
  return new Error('internal error');
}

function assert(condition: boolean): void {
  if (!condition) throw internalError();
}

// JSG unwrapping of a jsg::JsUint8Array method parameter.
function unwrapUint8Array(
  value: unknown,
  memberName: string,
  argumentIndex: number
): Uint8Array {
  if (!isUint8Array(value)) {
    throw new TypeError(
      `Failed to execute '${memberName}' on '${kResourceName}': parameter ` +
        `${argumentIndex + 1} is not of type 'Uint8Array'.`
    );
  }
  return value;
}

// JSG unwrapping of a jsg::JsString: coerces with ToString.
function unwrapString(value: unknown): string {
  // A template literal applies ToString, which, unlike String(), throws for
  // Symbols.
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-template-expression
  return typeof value === 'string' ? value : `${value as string}`;
}

// JSG unwrapping of a kj::OneOf<jsg::JsString, jsg::JsUint8Array>. The
// uncoercible Uint8Array is tried first; anything else becomes a string.
function unwrapStringOrUint8Array(value: unknown): string | Uint8Array {
  if (isUint8Array(value)) return value;
  return unwrapString(value);
}

// JSG unwrapping of a uint32_t.
function unwrapUint32(value: unknown): number {
  if (typeof value === 'number' && value >>> 0 === value) {
    return value;
  }
  const number = toNumber(value);
  if (!Number.isFinite(number)) {
    throw new TypeError(
      'The value cannot be converted because it is not an integer.'
    );
  }
  if (!(number >= 0)) {
    throw new TypeError(
      'The value cannot be converted because it is negative and this ' +
        'API expects a positive number.'
    );
  }
  if (number > 0xffffffff) {
    throw new TypeError(
      'Value out of range. Must be less than or equal to 4294967295.'
    );
  }
  return Math.trunc(number);
}

// JSG unwrapping of an int (int32_t).
function unwrapInt32(value: unknown): number {
  if (typeof value === 'number' && (value | 0) === value) {
    return value;
  }
  const number = toNumber(value);
  if (!Number.isFinite(number)) {
    return 0;
  }
  if (!(number <= 2147483647 && number >= -2147483648)) {
    throw new TypeError(
      'Value out of range. Must be between -2147483648 and 2147483647 (inclusive).'
    );
  }
  return Math.trunc(number);
}

// JSG unwrapping of a uint8_t (EncodingValue).
function unwrapUint8(value: unknown): number {
  const number = toNumber(value);
  if (!Number.isFinite(number)) {
    throw new TypeError(
      'The value cannot be converted because it is not an integer.'
    );
  }
  if (!(number >= 0)) {
    throw new TypeError(
      'The value cannot be converted because it is negative and this ' +
        'API expects a positive number.'
    );
  }
  if (number > 0xff) {
    throw new TypeError(
      'Value out of range. Must be less than or equal to 255.'
    );
  }
  return Math.trunc(number);
}

// JSG unwrapping of a jsg::Optional<T>.
function unwrapOptional<T>(
  value: unknown,
  unwrap: (value: unknown) => T
): T | undefined {
  return value === undefined ? undefined : unwrap(value);
}

// JSG unwrapping of a kj::Array<jsg::JsUint8Array>.
function unwrapUint8ArrayList(
  value: unknown,
  memberName: string,
  argumentIndex: number
): Uint8Array[] {
  if (!Array.isArray(value)) {
    throw new TypeError(
      `Failed to execute '${memberName}' on '${kResourceName}': parameter ` +
        `${argumentIndex + 1} is not of type 'Array'.`
    );
  }
  const list = value as unknown[];
  const result: Uint8Array[] = [];
  for (let i = 0; i < list.length; i++) {
    const element = list[i];
    if (!isUint8Array(element)) {
      throw new TypeError(
        `Incorrect type for array element ${i}: the provided value is not ` +
          `of type 'Uint8Array'.`
      );
    }
    result.push(element);
  }
  return result;
}

// jsg::JsUint8Array::create
function createUint8Array(length: number): Uint8Array {
  if (!(length < kMaxByteLength)) {
    throw new RangeError('The length is too large');
  }
  try {
    return new Uint8Array(length);
  } catch {
    throw new RangeError('Failed to allocate memory for Uint8Array');
  }
}

// jsg::JsUint8Array::slice(js, newLength): a view of the first `newLength`
// bytes, sharing the same backing store.
function sliceUint8Array(array: Uint8Array, newLength: number): Uint8Array {
  if (newLength > array.length) {
    throw new RangeError('New length exceeds array length');
  }
  return array.subarray(0, newLength);
}

// kj::ArrayPtr<T>::fill(kj::ArrayPtr<const T>)
function fillArray(dst: Uint8Array, src: Uint8Array): void {
  const otherSize = src.length;
  let counter = 0;
  for (let s = dst.length, i = 0; i < s; i++) {
    dst[i] = src[counter]!;
    if (++counter === otherSize) counter = 0;
  }
}

// jsg::JsString::toString: the string as UTF-8, without replacing invalid
// sequences.
function jsStringToString(string: string): Uint8Array {
  const buf = new Uint8Array(native.utf8Length(string) + 1);
  native.writeUtf8(string, buf, native.WRITE_NULL_TERMINATION);
  return buf.subarray(0, buf.length - 1);
}

// =======================================================================================

function tryFromHexDigit(c: number): number | undefined {
  if (0x30 /* '0' */ <= c && c <= 0x39 /* '9' */) {
    return c - 0x30;
  } else if (0x61 /* 'a' */ <= c && c <= 0x66 /* 'f' */) {
    return c - (0x61 - 10);
  } else if (0x41 /* 'A' */ <= c && c <= 0x46 /* 'F' */) {
    return c - (0x41 - 10);
  }
  return undefined;
}

function decodeHexTruncated(text: Uint8Array, strict = false): Uint8Array {
  // We do not use kj::decodeHex because we need to match Node.js'
  // behavior of truncating the response at the first invalid hex
  // pair as opposed to just marking that an error happened and
  // trying to continue with the decode.
  if (text.length % 2 !== 0) {
    if (strict) throw new TypeError('The text is not valid hex');
    text = text.subarray(0, text.length - 1);
  }
  const vec = createUint8Array(text.length / 2);
  let len = 0;

  for (let i = 0; i < text.length; i += 2) {
    let b = 0;
    const d1 = tryFromHexDigit(text[i]!);
    if (d1 !== undefined) {
      b = d1 << 4;
    } else {
      if (strict) throw new TypeError('The text is not valid hex');
      break;
    }
    const d2 = tryFromHexDigit(text[i + 1]!);
    if (d2 !== undefined) {
      b |= d2;
    } else {
      if (strict) throw new TypeError('The text is not valid hex');
      break;
    }
    vec[len++] = b;
  }

  if (len === vec.length) {
    return vec;
  }

  return sliceUint8Array(vec, len);
}

function writeInto(
  buffer: Uint8Array,
  string: string,
  offset: number,
  length: number,
  encoding: Encoding
): number {
  assert(offset <= buffer.length);
  assert(length <= buffer.length - offset);
  const dest = buffer.subarray(
    offset,
    Math.min(offset + length, buffer.length)
  );
  if (dest.length === 0 || string.length === 0) {
    return 0;
  }

  const flags = native.WRITE_REPLACE_INVALID_UTF8;

  switch (encoding) {
    case ASCII:
    // Falls through
    case LATIN1: {
      return native.writeOneByte(string, dest, flags);
    }
    case UTF8: {
      return native.writeUtf8(string, dest, flags);
    }
    case UTF16LE: {
      return native.writeUtf16(string, dest, flags) * 2;
    }
    case BASE64:
    // Falls through
    case BASE64URL: {
      const str = jsStringToString(string);
      return native.nbytesBase64Decode(dest, str);
    }
    case HEX: {
      const buf = new Uint8Array(string.length);
      native.writeOneByte(string, buf, flags);
      const backing = decodeHexTruncated(buf, false);
      const amountToCopy = Math.min(backing.length, dest.length);
      dest.set(backing.subarray(0, amountToCopy));
      return amountToCopy;
    }
    default:
      throw internalError();
  }
}

function decodeStringImpl(
  string: string,
  encoding: Encoding,
  strict = false
): Uint8Array {
  const length = string.length;
  if (length === 0) {
    return createUint8Array(0);
  }

  const options = native.WRITE_REPLACE_INVALID_UTF8;

  switch (encoding) {
    case ASCII:
    // Falls through
    case LATIN1: {
      const dest = createUint8Array(length);
      writeInto(dest, string, 0, dest.length, LATIN1);
      return dest;
    }
    case UTF8: {
      const dest = createUint8Array(native.utf8Length(string));
      writeInto(dest, string, 0, dest.length, UTF8);
      return dest;
    }
    case UTF16LE: {
      const dest = createUint8Array(length * 2);
      writeInto(dest, string, 0, dest.length, UTF16LE);
      return dest;
    }
    case BASE64:
    // Falls through
    case BASE64URL: {
      // We do not use the kj::String conversion here because inline null-characters
      // need to be ignored.
      const buf = new Uint8Array(length);
      const len = native.writeOneByte(string, buf, options);
      const text = buf.subarray(0, len);
      const dest = createUint8Array(
        native.simdutfMaximalBinaryLengthFromBase64(text)
      );
      const count = native.simdutfBase64ToBinary(text, dest);
      if (count < dest.length) {
        return sliceUint8Array(dest, count);
      }
      return dest;
    }
    case HEX: {
      const buf = new Uint8Array(length);
      native.writeOneByte(string, buf, options);
      return decodeHexTruncated(buf, strict);
    }
    default:
      throw internalError();
  }
}

export function byteLength(str: string): number {
  str = unwrapString(str);
  return native.utf8Length(str) >>> 0;
}

interface CompareOptions {
  aStart?: number;
  aEnd?: number;
  bStart?: number;
  bEnd?: number;
}

// JSG unwrapping of a jsg::Optional<CompareOptions>, a JSG_STRUCT.
function unwrapCompareOptions(
  value: unknown,
  memberName: string,
  argumentIndex: number
): Required<{ [K in keyof CompareOptions]: number | undefined }> | undefined {
  if (value === undefined) return undefined;
  if (value === null || typeof value !== 'object') {
    throw new TypeError(
      `Failed to execute '${memberName}' on '${kResourceName}': parameter ` +
        `${argumentIndex + 1} is not of type 'CompareOptions'.`
    );
  }
  const options = value as CompareOptions;
  return {
    aStart: unwrapOptional(options.aStart, unwrapUint32),
    aEnd: unwrapOptional(options.aEnd, unwrapUint32),
    bStart: unwrapOptional(options.bStart, unwrapUint32),
    bEnd: unwrapOptional(options.bEnd, unwrapUint32),
  };
}

export function compare(
  one: Uint8Array,
  two: Uint8Array,
  maybeOptions?: CompareOptions
): number {
  one = unwrapUint8Array(one, 'compare', 0);
  two = unwrapUint8Array(two, 'compare', 1);
  const options = unwrapCompareOptions(maybeOptions, 'compare', 2);

  let ptrOne = one;
  let ptrTwo = two;

  // The options allow comparing subranges within the two inputs.
  if (options !== undefined) {
    let end = options.aEnd ?? ptrOne.length;
    end = Math.min(end, ptrOne.length);
    let start = Math.min(end, options.aStart ?? 0);
    ptrOne = ptrOne.subarray(start, end);
    end = options.bEnd ?? ptrTwo.length;
    end = Math.min(end, ptrTwo.length);
    start = Math.min(end, options.bStart ?? 0);
    ptrTwo = ptrTwo.subarray(start, end);
  }

  const toCompare = Math.min(ptrOne.length, ptrTwo.length);
  // memcmp
  let result = 0;
  for (let i = 0; i < toCompare; i++) {
    const a = ptrOne[i]!;
    const b = ptrTwo[i]!;
    if (a !== b) {
      result = a - b;
      break;
    }
  }

  if (result === 0) {
    if (ptrOne.length > ptrTwo.length) return 1;
    else if (ptrOne.length < ptrTwo.length) return -1;
    else return 0;
  }

  return result > 0 ? 1 : -1;
}

export function concat(list: Uint8Array[], length: number): Uint8Array {
  // The Node.js Buffer.concat is interesting in that it doesn't just append
  // the buffers together as is. The length parameter is used to determine the
  // length of the result which can be lesser or greater than the actual
  // combined lengths of the inputs. If the length is lesser, the result will
  // be a truncated version of the combined buffers. If the length is greater,
  // the result will be the combined buffers with the remaining space filled
  // with zeroes.
  list = unwrapUint8ArrayList(list, 'concat', 0);
  length = unwrapUint32(length);

  if (!(length <= kMaxByteLength)) {
    throw new RangeError('The length is too large');
  }

  const dest = createUint8Array(length);
  if (length > 0) {
    let offset = 0;

    for (const src of list) {
      // The length of a Uint8Array is not cached, so we don't need to worry
      // about whether the underlying ArrayBuffer is detached or resized, etc.
      if (src.length === 0) continue;
      // The amount to copy is the lesser of the remaining space in the destination or
      // the size of the chunk we're copying.
      const amountToCopy = Math.min(src.length, dest.length - offset);
      dest.set(src.subarray(0, amountToCopy), offset);
      offset += amountToCopy;
      // If there's no more space in the destination, we're done.
      if (offset === dest.length) {
        break;
      }
    }
  }

  return dest;
}

export function decodeString(string: string, encoding: Encoding): Uint8Array {
  string = unwrapString(string);
  encoding = unwrapUint8(encoding);
  return decodeStringImpl(string, encoding);
}

export function fillImpl(
  buffer: Uint8Array,
  value: string | ArrayBufferView | ArrayBuffer,
  start: number,
  end: number,
  encoding?: Encoding
): void {
  buffer = unwrapUint8Array(buffer, 'fillImpl', 0);
  const unwrappedValue = unwrapStringOrUint8Array(value);
  start = unwrapUint32(start);
  end = unwrapUint32(end);
  const maybeEncoding = unwrapOptional(encoding, unwrapUint8);

  end = Math.min(end, buffer.length);
  if (end <= start) return;

  const ptr = buffer.subarray(start, end);
  if (typeof unwrappedValue === 'string') {
    const enc = maybeEncoding ?? UTF8;
    const decoded = decodeStringImpl(unwrappedValue, enc, true /* strict */);
    if (decoded.length === 0) {
      ptr.fill(0);
      return;
    }
    fillArray(ptr, decoded);
  } else {
    const source = unwrappedValue;
    if (source.length === 0) {
      ptr.fill(0);
      return;
    }
    fillArray(ptr, source);
  }
}

// Computes the offset for starting an indexOf or lastIndexOf search.
// Returns either a valid offset in [0...<length - 1>], ie inside the Buffer,
// or -1 to signal that there is no possible match.
function indexOfOffset(
  length: number,
  offset: number,
  needleLength: number,
  isForward: boolean
): number {
  const len = length | 0;
  needleLength = needleLength | 0;
  if (offset < 0) {
    if (((offset + len) | 0) >= 0) {
      // Negative offsets count backwards from the end of the buffer.
      return (len + offset) | 0;
    } else if (isForward || needleLength === 0) {
      // indexOf from before the start of the buffer: search the whole buffer.
      return 0;
    } else {
      // lastIndexOf from before the start of the buffer: no match.
      return -1;
    }
  } else {
    if (offset + needleLength <= len) {
      // Valid positive offset.
      return offset;
    } else if (needleLength === 0) {
      // Out of buffer bounds, but empty needle: point to end of buffer.
      return len;
    } else if (isForward) {
      // indexOf from past the end of the buffer: no match.
      return -1;
    } else {
      // lastIndexOf from past the end of the buffer: search the whole buffer.
      return (len - 1) | 0;
    }
  }
}

// Copies `bytes` into a new, aligned array of `uint16_t`s.
function toAlignedUint16Array(bytes: Uint8Array): Uint16Array {
  const aligned = new Uint16Array(bytes.length / 2);
  new Uint8Array(aligned.buffer).set(bytes);
  return aligned;
}

function indexOfBuffer(
  hayStack: Uint8Array,
  needle: Uint8Array,
  byteOffset: number,
  encoding: Encoding,
  isForward: boolean
): number | undefined {
  const enc = encoding;
  // Round down to the nearest multiple of 2 in case of UCS2.
  const hayStackLength =
    enc === UTF16LE ? hayStack.length - (hayStack.length % 2) : hayStack.length;
  const optOffset = indexOfOffset(
    hayStackLength,
    byteOffset,
    needle.length,
    isForward
  );

  if (needle.length === 0) return optOffset >>> 0;
  if (
    hayStackLength === 0 ||
    optOffset <= -1 ||
    (isForward && needle.length + optOffset > hayStackLength) ||
    needle.length > hayStackLength
  ) {
    return undefined;
  }
  let result = hayStackLength;
  if (enc === UTF16LE) {
    if (hayStackLength < 2 || needle.length < 2) {
      return undefined;
    }
    // Copy haystack and needle to aligned buffers, since the data may have an
    // odd byte offset.
    const hayStackU16Len = hayStackLength / 2;
    const alignedHayStack = toAlignedUint16Array(
      hayStack.subarray(0, hayStackLength)
    );

    const needleLen = needle.length - (needle.length % 2);
    const needleU16Len = needleLen / 2;
    const alignedNeedle = toAlignedUint16Array(needle.subarray(0, needleLen));

    result = searchString(
      alignedHayStack,
      hayStackU16Len,
      alignedNeedle,
      needleU16Len,
      Math.trunc(optOffset / 2),
      isForward
    );
    result *= 2;
  } else {
    result = searchString(
      hayStack,
      hayStack.length,
      needle,
      needle.length,
      optOffset,
      isForward
    );
  }

  if (result === hayStackLength) return undefined;

  return result >>> 0;
}

function indexOfString(
  hayStack: Uint8Array,
  needle: string,
  byteOffset: number,
  encoding: Encoding,
  isForward: boolean
): number | undefined {
  const enc = encoding;
  const decodedNeedle = decodeStringImpl(needle, enc);

  // Round down to the nearest multiple of 2 in case of UCS2
  const hayStackLength =
    enc === UTF16LE ? hayStack.length - (hayStack.length % 2) : hayStack.length;
  const optOffset = indexOfOffset(
    hayStackLength,
    byteOffset,
    decodedNeedle.length,
    isForward
  );

  if (decodedNeedle.length === 0) {
    return optOffset >>> 0;
  }

  if (
    hayStackLength === 0 ||
    optOffset <= -1 ||
    (isForward && decodedNeedle.length + optOffset > hayStackLength) ||
    decodedNeedle.length > hayStackLength
  ) {
    return undefined;
  }

  let result = hayStackLength;

  if (enc === UTF16LE) {
    if (hayStackLength < 2 || decodedNeedle.length < 2) {
      return undefined;
    }
    // Copy haystack to an aligned buffer, since the data may have an odd
    // byte offset.
    const hayStackU16Len = hayStackLength / 2;
    const alignedHayStack = toAlignedUint16Array(
      hayStack.subarray(0, hayStackLength)
    );

    // decodedNeedle is a freshly allocated, and therefore aligned, array.
    result = searchString(
      alignedHayStack,
      hayStackU16Len,
      new Uint16Array(
        decodedNeedle.buffer,
        decodedNeedle.byteOffset,
        decodedNeedle.length / 2
      ),
      decodedNeedle.length / 2,
      Math.trunc(optOffset / 2),
      isForward
    );
    result *= 2;
  } else {
    result = searchString(
      hayStack,
      hayStack.length,
      decodedNeedle,
      decodedNeedle.length,
      optOffset,
      isForward
    );
  }

  if (result === hayStackLength) return undefined;

  return result >>> 0;
}

function toStringImpl(
  bytes: Uint8Array,
  start: number,
  end: number,
  encoding: Encoding
): string {
  assert(end <= bytes.length);
  if (end < start) end = start;
  const slice = bytes.subarray(start, end);
  if (slice.length === 0) return '';
  switch (encoding) {
    case ASCII: {
      // TODO(perf): We can look at making this more performant later.
      // Essentially we have to modify the buffer such that every byte
      // has the highest bit turned off. Whee! Node.js has a faster
      // algorithm that it implements so we can likely adopt that.
      const copy = new Uint8Array(slice.length);
      for (let i = 0; i < slice.length; i++) {
        copy[i] = slice[i]! & 0x7f;
      }
      return native.newFromOneByte(copy);
    }
    case LATIN1: {
      return native.newFromOneByte(slice);
    }
    case UTF8: {
      return native.newFromUtf8(slice);
    }
    case UTF16LE: {
      return native.newFromTwoByte(slice);
    }
    case BASE64: {
      const length = native.simdutfBase64LengthFromBinary(slice.length);
      const out = new Uint8Array(length);
      native.simdutfBinaryToBase64(slice, out);
      return native.newFromOneByte(out);
    }
    case BASE64URL: {
      const length = native.simdutfBase64UrlLengthFromBinary(slice.length);
      const out = new Uint8Array(length);
      native.simdutfBinaryToBase64Url(slice, out);
      return native.newFromOneByte(out);
    }
    case HEX: {
      return native.newFromUtf8(native.kjEncodeHex(slice));
    }
    default:
      throw internalError();
  }
}

export function indexOf(
  buffer: Uint8Array,
  value: string | Uint8Array,
  byteOffset?: number,
  encoding?: Encoding,
  isForward?: boolean
): number | undefined {
  buffer = unwrapUint8Array(buffer, 'indexOf', 0);
  const unwrappedValue = unwrapStringOrUint8Array(value);
  const offset = unwrapInt32(byteOffset);
  const enc = unwrapUint8(encoding);
  const forward = !!isForward;

  if (typeof unwrappedValue === 'string') {
    return indexOfString(buffer, unwrappedValue, offset, enc, forward);
  }
  return indexOfBuffer(buffer, unwrappedValue, offset, enc, forward);
}

export function swap(buffer: Uint8Array, size: 16 | 32 | 64): void {
  buffer = unwrapUint8Array(buffer, 'swap', 0);
  const unwrappedSize = unwrapInt32(size);
  if (buffer.length <= 1) return;
  switch (unwrappedSize) {
    case 16: {
      if (!native.nbytesSwapBytes16(buffer)) {
        throw new Error('Swap bytes failed');
      }
      break;
    }
    case 32: {
      if (!native.nbytesSwapBytes32(buffer)) {
        throw new Error('Swap bytes failed');
      }
      break;
    }
    case 64: {
      if (!native.nbytesSwapBytes64(buffer)) {
        throw new Error('Swap bytes failed');
      }
      break;
    }
    default:
      throw new Error('Unreachable');
  }
}

export function toString(
  bytes: Uint8Array,
  start: number,
  end: number,
  encoding: Encoding
): string {
  bytes = unwrapUint8Array(bytes, 'toString', 0);
  start = unwrapUint32(start);
  end = unwrapUint32(end);
  encoding = unwrapUint8(encoding);
  end = Math.min(bytes.length, end);
  if (end <= start) return '';
  return toStringImpl(bytes, start, end, encoding);
}

export function write(
  buffer: Uint8Array,
  string: string,
  offset: number,
  length: number,
  encoding: Encoding
): number {
  buffer = unwrapUint8Array(buffer, 'write', 0);
  string = unwrapString(string);
  offset = unwrapUint32(offset);
  length = unwrapUint32(length);
  encoding = unwrapUint8(encoding);
  // In the C++, `buffer.size() - offset` is unsigned and wraps when offset is
  // past the end, leaving `length` unchanged; writeInto then rejects it.
  if (offset <= buffer.length) {
    length = Math.min(length, buffer.length - offset);
  }
  if (length === 0) return 0;
  return writeInto(buffer, string, offset, length, encoding) >>> 0;
}

// ======================================================================================
// StringDecoder
//
// It's helpful to review a bit about how the implementation works here.
//
// StringDecoder is a streaming decoder that ensures that multi-byte characters are correctly
// handled. So, for instance, let's suppose I have the utf8 bytes for a euro symbol (0xe2, 0x82,
// 0xac), but I only get those one at a time... StringDecoder will ensure that those are correctly
// handled over multiple calls to write(...)...
//
//   const sd = new StringDecoder();
//   let results = '';
//   results += sd.write(new Uint8Array([0xe2]));  // results.length === 0
//   results += sd.write(new Uint8Array([0x82]));  // results.length === 0
//   results += sd.write(new Uint8Array([0xac]));  // results.length === 1
//   results += sd.end();
//
// Internally, the decoder allocates a small 7 byte buffer (the state) argument below.
//
// The first four bytes of the state are used to hold partial bytes received on the previous
// write. The fifth byte in state is a count of the number of missing bytes we need to complete
// the character. The sixth byte in state is the number of bytes that have been encoded into the
// first four. The seventh byte in state identifies the Encoding and matches the values of the
// Encoding enum.
//
// So, in our example above, initially the first six bytes of the state are [0x00, 0x00, 0x00,
// 0x00, 0x00, 0x00]
//
// After the first call to write above, the state is updated to: [0xe2, 0x00, 0x00, 0x00, 0x02,
// 0x01]
//
// After the second call to write, the state is updated to: [0xe2, 0x82, 0x00, 0x00, 0x01, 0x02]
//
// After the third call to write, the pending multibyte character is completed, the state becomes:
// [0xe2, 0x82, 0xac, 0x00, 0x00, 0x00] ... while the bytes are still in state, the buffered bytes
// and bytes needed are zeroed out. Since the character is completed on that third write, it is
// included in the returned string.
//
// The implementation here is taken nearly verbatim from Node.js with a few adaptations. The code
// from Node.js has remained largely unchanged for years and is well-proven.
//
// The state is a Uint8Array, so, as with the kj::byte fields in the C++, arithmetic on its
// elements wraps modulo 256 when stored.

function getMissingBytes(state: Uint8Array): number {
  if (!(state[kMissingBytes]! <= kIncompleteCharactersEnd)) {
    throw new Error('Missing bytes cannot exceed 4');
  }
  return state[kMissingBytes]!;
}

function getBufferedBytes(state: Uint8Array): number {
  if (!(state[kBufferedBytes]! <= kIncompleteCharactersEnd)) {
    throw new Error('Buffered bytes cannot exceed 4');
  }
  return state[kBufferedBytes]!;
}

function getEncoding(state: Uint8Array): Encoding {
  if (!(state[kEncoding]! <= HEX)) {
    throw new Error('Invalid StringDecoder state');
  }
  return state[kEncoding]!;
}

function getBufferedString(state: Uint8Array): string {
  if (!(getBufferedBytes(state) <= kIncompleteCharactersEnd)) {
    throw new Error('Invalid StringDecoder state');
  }
  const ret = toStringImpl(
    state,
    kIncompleteCharactersStart,
    kIncompleteCharactersStart + getBufferedBytes(state),
    getEncoding(state)
  );
  state[kBufferedBytes] = 0;
  return ret;
}

export function decode(bytes: Uint8Array, state: Uint8Array): string {
  bytes = unwrapUint8Array(bytes, 'decode', 0);
  state = unwrapUint8Array(state, 'decode', 1);
  if (state.length !== kSize) {
    throw new TypeError('Invalid StringDecoder');
  }
  const enc = getEncoding(state);
  if (enc === ASCII || enc === LATIN1 || enc === HEX) {
    // For ascii, latin1, and hex, we can just use the regular
    // toString option since there will never be a case where
    // these have left-over characters.
    return toStringImpl(bytes, 0, bytes.length, enc);
  }

  let prepend = '';
  let body = '';
  let nread = bytes.length;

  // If bytes is empty there's nothing to decode.
  if (bytes.length === 0) return '';

  // `data` in the C++ is a pointer into `bytes`; here it is an offset.
  let data = 0;

  if (getMissingBytes(state) > 0) {
    if (
      !(
        getMissingBytes(state) + getBufferedBytes(state) <=
        kIncompleteCharactersEnd
      )
    ) {
      throw new Error('Invalid StringDecoder state');
    }
    if (enc === UTF8) {
      // For UTF-8, we need special treatment to align with the V8 decoder:
      // If an incomplete character is found at a chunk boundary, we use
      // its remainder and pass it to V8 as-is.
      for (let i = 0; i < nread && i < getMissingBytes(state); ++i) {
        if ((bytes[data + i]! & 0xc0) !== 0x80) {
          // This byte is not a continuation byte even though it should have
          // been one. We stop decoding of the incomplete character at this
          // point (but still use the rest of the incomplete bytes from this
          // chunk) and assume that the new, unexpected byte starts a new one.
          state[kMissingBytes] = 0;
          // TypedArray.prototype.set has memmove semantics: bytes may alias the
          // incomplete character buffer (e.g. when the caller passes
          // decoder.lastChar as input).
          state.set(
            bytes.subarray(data, data + i),
            kIncompleteCharactersStart + getBufferedBytes(state)
          );
          state[kBufferedBytes] = state[kBufferedBytes]! + i;
          data += i;
          nread -= i;
          break;
        }
      }
    }

    const foundBytes = Math.min(nread, getMissingBytes(state));
    // memmove, as above.
    state.set(
      bytes.subarray(data, data + foundBytes),
      kIncompleteCharactersStart + getBufferedBytes(state)
    );
    // Adjust the two buffers.
    data += foundBytes;
    nread -= foundBytes;

    state[kMissingBytes] = state[kMissingBytes]! - foundBytes;
    state[kBufferedBytes] = state[kBufferedBytes]! + foundBytes;

    if (getMissingBytes(state) === 0) {
      // If no more bytes are missing, create a small string that we will later prepend.
      prepend = getBufferedString(state);
    }
  }

  if (nread === 0) {
    body = prepend.length ? prepend : '';
    prepend = '';
  } else {
    if (getMissingBytes(state) !== 0) {
      throw new Error('Invalid StringDecoder state');
    }
    if (getBufferedBytes(state) !== 0) {
      throw new Error('Invalid StringDecoder state');
    }

    // See whether there is a character that we may have to cut off and
    // finish when receiving the next chunk.
    if (enc === UTF8 && bytes[data + nread - 1]! & 0x80) {
      // This is UTF-8 encoded data and we ended on a non-ASCII UTF-8 byte.
      // This means we'll need to figure out where the character to which
      // the byte belongs begins.
      for (let i = nread - 1; ; --i) {
        if (!(i >= 0 && i < nread)) {
          throw new Error('Invalid StringDecoder state');
        }
        state[kBufferedBytes] = state[kBufferedBytes]! + 1;
        if ((bytes[data + i]! & 0xc0) === 0x80) {
          // This byte does not start a character (a "trailing" byte).
          if (state[kBufferedBytes]! >= 4 || i === 0) {
            // We either have more then 4 trailing bytes (which means
            // the current character would not be inside the range for
            // valid Unicode, and in particular cannot be represented
            // through JavaScript's UTF-16-based approach to strings), or the
            // current buffer does not contain the start of an UTF-8 character
            // at all. Either way, this is invalid UTF8 and we can just
            // let the engine's decoder handle it.
            state[kBufferedBytes] = 0;
            break;
          }
        } else {
          // Found the first byte of a UTF-8 character. By looking at the
          // upper bits we can tell how long the character *should* be.
          const lead = bytes[data + i]!;
          if ((lead & 0xe0) === 0xc0) {
            state[kMissingBytes] = 2;
          } else if ((lead & 0xf0) === 0xe0) {
            state[kMissingBytes] = 3;
          } else if ((lead & 0xf8) === 0xf0) {
            state[kMissingBytes] = 4;
          } else {
            // This lead byte would indicate a character outside of the
            // representable range.
            state[kBufferedBytes] = 0;
            break;
          }

          if (getBufferedBytes(state) >= getMissingBytes(state)) {
            // Received more or exactly as many trailing bytes than the lead
            // character would indicate. In the "==" case, we have valid
            // data and don't need to slice anything off;
            // in the ">" case, this is invalid UTF-8 anyway.
            state[kMissingBytes] = 0;
            state[kBufferedBytes] = 0;
          }

          state[kMissingBytes] = state[kMissingBytes]! - state[kBufferedBytes]!;
          break;
        }
      }
    } else if (enc === UTF16LE) {
      if (nread % 2 === 1) {
        // We got half a codepoint, and need the second byte of it.
        state[kBufferedBytes] = 1;
        state[kMissingBytes] = 1;
      } else if ((bytes[data + nread - 1]! & 0xfc) === 0xd8) {
        // Half a split UTF-16 character.
        state[kBufferedBytes] = 2;
        state[kMissingBytes] = 2;
      }
    } else if (enc === BASE64 || enc === BASE64URL) {
      state[kBufferedBytes] = nread % 3;
      if (state[kBufferedBytes]! > 0) {
        state[kMissingBytes] = 3 - getBufferedBytes(state);
      }
    }

    if (getBufferedBytes(state) > 0) {
      // Copy the requested number of buffered bytes from the end of the
      // input into the incomplete character buffer.
      nread -= getBufferedBytes(state);
      // memmove, as above.
      state.set(
        bytes.subarray(data + nread, data + nread + getBufferedBytes(state)),
        kIncompleteCharactersStart
      );
    }

    if (nread > 0) {
      body = toStringImpl(bytes.subarray(data, data + nread), 0, nread, enc);
    } else {
      body = '';
    }
  }

  if (prepend.length === 0) {
    return body;
  } else {
    return prepend + body;
  }
}

export function flush(state: Uint8Array): string {
  state = unwrapUint8Array(state, 'flush', 0);
  if (state.length !== kSize) {
    throw new TypeError('Invalid StringDecoder');
  }
  const enc = getEncoding(state);
  if (enc === ASCII || enc === HEX || enc === LATIN1) {
    if (getMissingBytes(state) !== 0) {
      throw new Error('Invalid StringDecoder state');
    }
    if (getBufferedBytes(state) !== 0) {
      throw new Error('Invalid StringDecoder state');
    }
  }

  if (enc === UTF16LE && getBufferedBytes(state) % 2 === 1) {
    // Ignore a single trailing byte, like the JS decoder does.
    state[kMissingBytes] = state[kMissingBytes]! - 1;
    state[kBufferedBytes] = state[kBufferedBytes]! - 1;
  }

  if (getBufferedBytes(state) === 0) {
    return '';
  }

  const ret = getBufferedString(state);
  state[kMissingBytes] = 0;

  return ret;
}

export function isAscii(bytes: ArrayBufferView): boolean {
  const buffer = unwrapUint8Array(bytes, 'isAscii', 0);
  if (buffer.length === 0) return true;
  return native.simdutfValidateAscii(buffer);
}

export function isUtf8(bytes: ArrayBufferView): boolean {
  const buffer = unwrapUint8Array(bytes, 'isUtf8', 0);
  if (buffer.length === 0) return true;
  return native.simdutfValidateUtf8(buffer);
}

export function transcode(
  source: ArrayBufferView,
  rawFromEncoding: Encoding,
  rawToEncoding: Encoding
): Uint8Array {
  const sourceBytes = unwrapUint8Array(source, 'transcode', 0);
  const fromEncoding = unwrapUint8(rawFromEncoding);
  const toEncoding = unwrapUint8(rawToEncoding);

  if (!(canBeTranscoded(fromEncoding) && canBeTranscoded(toEncoding))) {
    throw new Error('Unable to transcode buffer due to unsupported encoding');
  }

  return native.transcode(sourceBytes, fromEncoding, toEncoding);
}

export default {
  byteLength,
  compare,
  concat,
  decodeString,
  fillImpl,
  indexOf,
  swap,
  toString,
  write,
  decode,
  flush,
  isAscii,
  isUtf8,
  transcode,
  ASCII,
  LATIN1,
  UTF8,
  UTF16LE,
  BASE64,
  BASE64URL,
  HEX,
};
