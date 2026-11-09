// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

// The TypeScript implementation of `node-internal:buffer`, selected by the
// NODEJS_BUFFER_TS autogate. It is a port of src/workerd/api/node/buffer.c++
// (BufferUtil) and follows it function by function, so the two can be
// compared side by side.
//
// Wherever the C++ calls into V8, libc, simdutf, nbytes, kj, i18n, or the
// string search in buffer-string-search.h, this module calls the equivalent
// primitive of `node-internal:buffer_native` (src/rust/api/buffer.rs), which
// calls V8 for string conversions and implements the rest in Rust.
// Everything else is ported directly.
//
// The C++ receives its arguments through JSG, which converts them to the
// declared C++ types and throws a TypeError on failure. The `unwrap*` helpers
// below reproduce those conversions and messages.

// Typed-array element reads below are always in bounds, as they are in the C++.
/* eslint-disable @typescript-eslint/no-non-null-assertion */

import { default as native } from 'node-internal:buffer_native';

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

// v8::Value::IsUint8Array, without a native call. The
// %TypedArray%.prototype[Symbol.toStringTag] getter returns the name of the
// typed array's type from its internal slot, and undefined for anything else,
// including Proxies. Node.js's util.types.isUint8Array works the same way.
// Like the module's other uses of built-ins, this relies on the getter being
// the original when the module loads. The getter is called with .call, so it
// is left unbound.
const typedArrayPrototype = Object.getPrototypeOf(
  Uint8Array.prototype
) as object;
// eslint-disable-next-line @typescript-eslint/unbound-method
const getTypedArrayName = Object.getOwnPropertyDescriptor(
  typedArrayPrototype,
  Symbol.toStringTag
)!.get as (this: unknown) => string | undefined;
// eslint-disable-next-line @typescript-eslint/unbound-method
const getTypedArrayLength = Object.getOwnPropertyDescriptor(
  typedArrayPrototype,
  'length'
)!.get as (this: Uint8Array) => number;
// eslint-disable-next-line @typescript-eslint/unbound-method
const getTypedArrayBuffer = Object.getOwnPropertyDescriptor(
  typedArrayPrototype,
  'buffer'
)!.get as (this: Uint8Array) => ArrayBufferLike;
// eslint-disable-next-line @typescript-eslint/unbound-method
const getTypedArrayByteOffset = Object.getOwnPropertyDescriptor(
  typedArrayPrototype,
  'byteOffset'
)!.get as (this: Uint8Array) => number;
function isUint8Array(value: unknown): value is Uint8Array {
  return getTypedArrayName.call(value) === 'Uint8Array';
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
  const length = list.length;
  const result: Uint8Array[] = [];
  for (let i = 0; i < length; i++) {
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
  if (newLength > getTypedArrayLength.call(array)) {
    throw new RangeError('New length exceeds array length');
  }
  return new Uint8Array(
    getTypedArrayBuffer.call(array),
    getTypedArrayByteOffset.call(array),
    newLength
  );
}

// Returns true if `a[aStart, aEnd)` and `b` share any bytes.
function overlaps(
  a: Uint8Array,
  aStart: number,
  aEnd: number,
  b: Uint8Array
): boolean {
  if (a.buffer !== b.buffer) return false;
  const start = a.byteOffset + aStart;
  const end = a.byteOffset + aEnd;
  return start < b.byteOffset + b.length && b.byteOffset < end;
}

// kj::ArrayPtr<T>::fill(kj::ArrayPtr<const T>) on `dst[start, end)`: repeats
// `src` over the range, one element at a time.
function fillArrayByElement(
  dst: Uint8Array,
  start: number,
  end: number,
  src: Uint8Array
): void {
  const otherSize = src.length;
  let counter = 0;
  for (let i = start; i < end; i++) {
    dst[i] = src[counter]!;
    if (++counter === otherSize) counter = 0;
  }
}

// fillArrayByElement for a `src` that does not overlap `dst[start, end)`: one
// copy of `src` is written and the filled prefix is then doubled with
// copyWithin, which writes the same bytes.
function fillArray(
  dst: Uint8Array,
  start: number,
  end: number,
  src: Uint8Array
): void {
  const size = end - start;
  const otherSize = src.length;
  const first = Math.min(otherSize, size);
  dst.set(first === otherSize ? src : src.subarray(0, first), start);
  for (let filled = first; filled < size;) {
    const n = Math.min(filled, size - filled);
    dst.copyWithin(start + filled, start, start + n);
    filled += n;
  }
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
  // `dest` in the C++ is the `[destStart, destEnd)` slice of `buffer`.
  const destStart = offset;
  const destEnd = Math.min(offset + length, buffer.length);
  if (destEnd - destStart === 0 || string.length === 0) {
    return 0;
  }

  switch (encoding) {
    case ASCII:
    // Falls through
    case LATIN1: {
      return destStart === 0 && destEnd === buffer.length
        ? native.writeOneByte(string, buffer)
        : native.writeOneByteRange(string, buffer, destStart, destEnd);
    }
    case UTF8: {
      return destStart === 0 && destEnd === buffer.length
        ? native.writeUtf8(string, buffer)
        : native.writeUtf8Range(string, buffer, destStart, destEnd);
    }
    case UTF16LE: {
      return (
        (destStart === 0 && destEnd === buffer.length
          ? native.writeUtf16(string, buffer)
          : native.writeUtf16Range(string, buffer, destStart, destEnd)) * 2
      );
    }
    case BASE64:
    // Falls through
    case BASE64URL: {
      return native.decodeBase64Into(buffer, destStart, destEnd, string);
    }
    case HEX: {
      const backing = native.decodeHex(string, false);
      const amountToCopy = Math.min(backing.length, destEnd - destStart);
      buffer.set(
        amountToCopy === backing.length
          ? backing
          : backing.subarray(0, amountToCopy),
        destStart
      );
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
      return native.decodeBase64(string);
    }
    case HEX: {
      return native.decodeHex(string, strict);
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

  // `ptrOne` and `ptrTwo` in the C++ are the `[oneStart, oneEnd)` and
  // `[twoStart, twoEnd)` slices of the inputs.
  let oneStart = 0;
  let oneEnd = one.length;
  let twoStart = 0;
  let twoEnd = two.length;

  // The options allow comparing subranges within the two inputs.
  if (options !== undefined) {
    oneEnd = Math.min(options.aEnd ?? one.length, one.length);
    oneStart = Math.min(oneEnd, options.aStart ?? 0);
    twoEnd = Math.min(options.bEnd ?? two.length, two.length);
    twoStart = Math.min(twoEnd, options.bStart ?? 0);
  }

  const oneLength = oneEnd - oneStart;
  const twoLength = twoEnd - twoStart;
  const toCompare = Math.min(oneLength, twoLength);
  const result =
    toCompare > 0 ? native.memcmp(one, oneStart, two, twoStart, toCompare) : 0;

  if (result === 0) {
    if (oneLength > twoLength) return 1;
    else if (oneLength < twoLength) return -1;
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
      // Read the current view length from its internal slot, ignoring own properties.
      const srcLength = getTypedArrayLength.call(src);
      if (srcLength === 0) continue;
      // The amount to copy is the lesser of the remaining space in the destination or
      // the size of the chunk we're copying.
      const amountToCopy = Math.min(srcLength, dest.length - offset);
      dest.set(
        amountToCopy === srcLength ? src : sliceUint8Array(src, amountToCopy),
        offset
      );
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

  // `ptr` in the C++ is the `[start, end)` slice of `buffer`.
  if (typeof unwrappedValue === 'string') {
    const enc = maybeEncoding ?? UTF8;
    const decoded = decodeStringImpl(unwrappedValue, enc, true /* strict */);
    if (decoded.length === 0) {
      buffer.fill(0, start, end);
      return;
    }
    // `decoded` is a new array, so it cannot overlap `buffer`.
    fillArray(buffer, start, end, decoded);
  } else {
    const source = unwrappedValue;
    if (source.length === 0) {
      buffer.fill(0, start, end);
      return;
    }
    if (overlaps(buffer, start, end, source)) {
      fillArrayByElement(buffer, start, end, source);
    } else {
      fillArray(buffer, start, end, source);
    }
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
    // searchTwoByte handles the alignment of the haystack and needle, since
    // the data may have an odd byte offset.
    const hayStackU16Len = hayStackLength / 2;
    const needleLen = needle.length - (needle.length % 2);
    const needleU16Len = needleLen / 2;

    result = native.searchTwoByte(
      hayStack,
      hayStackU16Len,
      needle,
      needleU16Len,
      Math.trunc(optOffset / 2),
      isForward
    );
    result *= 2;
  } else {
    result = native.searchOneByte(hayStack, needle, optOffset, isForward);
  }

  if (result === hayStackLength) return undefined;

  return result >>> 0;
}

// decodeStringImpl for a search needle. A needle whose decoded form fits in
// kNeedleScratchSize bytes is written into one array shared by every search,
// instead of a new array per search. The shared array is too large for V8 to
// keep on the JS heap, so the search native can read it without V8 first
// moving it off the heap. The result must not be kept past the search, and no
// JavaScript may run while it is in use; both hold because it is only passed
// to searchOneByte or searchTwoByte. Base64 and hex needles are not shared,
// since decodeStringImpl already uses scratch space for them.
const kNeedleScratchSize = 256;
let needleScratch: Uint8Array | undefined;
function decodeNeedle(needle: string, enc: Encoding): Uint8Array {
  // The most bytes each encoding writes per UTF-16 code unit.
  let maxLength: number;
  switch (enc) {
    case ASCII:
    // Falls through
    case LATIN1:
      maxLength = needle.length;
      break;
    case UTF16LE:
      maxLength = needle.length * 2;
      break;
    case UTF8:
      maxLength = needle.length * 3;
      break;
    default:
      return decodeStringImpl(needle, enc);
  }
  if (needle.length === 0 || maxLength > kNeedleScratchSize) {
    return decodeStringImpl(needle, enc);
  }
  const scratch = (needleScratch ??= new Uint8Array(kNeedleScratchSize));
  // The same writes as writeInto, into the start of `scratch`.
  let length: number;
  switch (enc) {
    case UTF16LE:
      length = native.writeUtf16Range(needle, scratch, 0, maxLength) * 2;
      break;
    case UTF8:
      length = native.writeUtf8Range(needle, scratch, 0, maxLength);
      break;
    default:
      length = native.writeOneByteRange(needle, scratch, 0, maxLength);
  }
  return scratch.subarray(0, length);
}

function indexOfString(
  hayStack: Uint8Array,
  needle: string,
  byteOffset: number,
  encoding: Encoding,
  isForward: boolean
): number | undefined {
  const enc = encoding;
  const decodedNeedle = decodeNeedle(needle, enc);

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
    // searchTwoByte handles the alignment of the haystack, since the data may
    // have an odd byte offset.
    const hayStackU16Len = hayStackLength / 2;

    result = native.searchTwoByte(
      hayStack,
      hayStackU16Len,
      decodedNeedle,
      decodedNeedle.length / 2,
      Math.trunc(optOffset / 2),
      isForward
    );
    result *= 2;
  } else {
    result = native.searchOneByte(
      hayStack,
      decodedNeedle,
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
  // `slice` in the C++ is the `[start, end)` slice of `bytes`.
  const sliceLength = end - start;
  if (sliceLength === 0) return '';
  switch (encoding) {
    case ASCII: {
      return native.newFromAscii(bytes, start, end);
    }
    case LATIN1: {
      return native.newFromOneByte(bytes, start, end);
    }
    case UTF8: {
      return native.newFromUtf8(bytes, start, end);
    }
    case UTF16LE: {
      return native.newFromTwoByte(bytes, start, end);
    }
    case BASE64: {
      return native.encodeBase64(bytes, start, end);
    }
    case BASE64URL: {
      return native.encodeBase64Url(bytes, start, end);
    }
    case HEX: {
      return native.encodeHex(bytes, start, end);
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
      if (!native.swapBytes16(buffer)) {
        throw new Error('Swap bytes failed');
      }
      break;
    }
    case 32: {
      if (!native.swapBytes32(buffer)) {
        throw new Error('Swap bytes failed');
      }
      break;
    }
    case 64: {
      if (!native.swapBytes64(buffer)) {
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

// memmove(state + dstOffset, bytes + srcStart, count) for the bytes of an
// incomplete character. `count` is at most kIncompleteCharactersEnd, so the
// bytes are read into locals before any is written, which is correct even
// when `bytes` aliases `state` (e.g. when the caller passes decoder.lastChar
// as input).
function copyIncompleteBytes(
  state: Uint8Array,
  dstOffset: number,
  bytes: Uint8Array,
  srcStart: number,
  count: number
): void {
  assert(count <= kIncompleteCharactersEnd);
  const b0 = count > 0 ? bytes[srcStart]! : 0;
  const b1 = count > 1 ? bytes[srcStart + 1]! : 0;
  const b2 = count > 2 ? bytes[srcStart + 2]! : 0;
  const b3 = count > 3 ? bytes[srcStart + 3]! : 0;
  if (count > 0) state[dstOffset] = b0;
  if (count > 1) state[dstOffset + 1] = b1;
  if (count > 2) state[dstOffset + 2] = b2;
  if (count > 3) state[dstOffset + 3] = b3;
}

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
          copyIncompleteBytes(
            state,
            kIncompleteCharactersStart + getBufferedBytes(state),
            bytes,
            data,
            i
          );
          state[kBufferedBytes] = state[kBufferedBytes]! + i;
          data += i;
          nread -= i;
          break;
        }
      }
    }

    const foundBytes = Math.min(nread, getMissingBytes(state));
    copyIncompleteBytes(
      state,
      kIncompleteCharactersStart + getBufferedBytes(state),
      bytes,
      data,
      foundBytes
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
      copyIncompleteBytes(
        state,
        kIncompleteCharactersStart,
        bytes,
        data + nread,
        getBufferedBytes(state)
      );
    }

    if (nread > 0) {
      body = toStringImpl(bytes, data, data + nread, enc);
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
  return native.validateAscii(buffer);
}

export function isUtf8(bytes: ArrayBufferView): boolean {
  const buffer = unwrapUint8Array(bytes, 'isUtf8', 0);
  return native.validateUtf8(buffer);
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
