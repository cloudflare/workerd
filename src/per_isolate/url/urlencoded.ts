'use strict';

// The application/x-www-form-urlencoded parser and serializer
// (https://url.spec.whatwg.org/#application/x-www-form-urlencoded), for
// URLSearchParams on the standard path. A leaf.
//
// Both take USVStrings, so encoding to UTF-8 never meets a lone surrogate.

import type { SearchParamsEntry } from './backend';
import type { InternalArrayHelpers } from './internal-array';

const {
  ArrayPrototypePush,
  StringPrototypeCharCodeAt,
  StringPrototypeIndexOf,
  StringPrototypeSlice,
  TextDecoder,
  TextDecoderDecode,
  TextEncoder,
  TextEncoderEncode,
  TypedArrayPrototypeGetLength,
  TypedArrayPrototypeSubarray,
} = primordials;

const { internalArray } = require('url/internal-array') as InternalArrayHelpers;

const kEncoder = new TextEncoder();
// UTF-8 decode without BOM: a leading U+FEFF is kept.
const kDecoder = new TextDecoder('utf-8', {
  __proto__: null,
  fatal: false,
  ignoreBOM: true,
} as TextDecoderOptions);

const kPercent = 0x25;
const kPlus = 0x2b;
const kSpace = 0x20;

function hexValue(byte: number): number {
  if (byte >= 0x30 && byte <= 0x39) return byte - 0x30;
  if (byte >= 0x41 && byte <= 0x46) return byte - 0x41 + 10;
  if (byte >= 0x61 && byte <= 0x66) return byte - 0x61 + 10;
  return -1;
}

// Replaces each '+' with a space.
function replacePlus(input: string): string {
  let result = '';
  let start = 0;
  let plus = StringPrototypeIndexOf(input, '+');
  while (plus !== -1) {
    result += `${StringPrototypeSlice(input, start, plus)} `;
    start = plus + 1;
    plus = StringPrototypeIndexOf(input, '+', start);
  }
  return result + StringPrototypeSlice(input, start);
}

// A name or value as the parser decodes it: '+' becomes a space, then the
// UTF-8 bytes are percent-decoded and decoded, with U+FFFD for invalid
// sequences. A '%' not followed by two hex digits stays as it is.
function decodeComponent(input: string): string {
  if (StringPrototypeIndexOf(input, '%') === -1) {
    return StringPrototypeIndexOf(input, '+') === -1
      ? input
      : replacePlus(input);
  }
  const bytes = TextEncoderEncode(kEncoder, input);
  const length = TypedArrayPrototypeGetLength(bytes);
  let out = 0;
  for (let i = 0; i < length; i++) {
    const byte = bytes[i] as number;
    if (byte === kPlus) {
      bytes[out++] = kSpace;
    } else if (byte === kPercent && i + 2 < length) {
      const high = hexValue(bytes[i + 1] as number);
      const low = hexValue(bytes[i + 2] as number);
      if (high === -1 || low === -1) {
        bytes[out++] = byte;
      } else {
        bytes[out++] = (high << 4) | low;
        i += 2;
      }
    } else {
      bytes[out++] = byte;
    }
  }
  return TextDecoderDecode(
    kDecoder,
    TypedArrayPrototypeSubarray(bytes, 0, out)
  );
}

// https://url.spec.whatwg.org/#concept-urlencoded-parser, on a string
// rather than bytes: the input's UTF-8 encoding is what is parsed.
function parseUrlencoded(input: string): SearchParamsEntry[] {
  const list = internalArray<SearchParamsEntry>();
  const length = input.length;
  let start = 0;
  while (start < length) {
    let end = StringPrototypeIndexOf(input, '&', start);
    if (end === -1) end = length;
    if (end > start) {
      const equals = StringPrototypeIndexOf(input, '=', start);
      let name: string;
      let value: string;
      if (equals !== -1 && equals < end) {
        name = StringPrototypeSlice(input, start, equals);
        value = StringPrototypeSlice(input, equals + 1, end);
      } else {
        name = StringPrototypeSlice(input, start, end);
        value = '';
      }
      ArrayPrototypePush(list, [decodeComponent(name), decodeComponent(value)]);
    }
    start = end + 1;
  }
  return list;
}

// Each byte as the serializer writes it: itself if it is outside the
// application/x-www-form-urlencoded percent-encode set, '+' for a space, else
// percent-encoded. Built while the bootstrap runs, before any user code, so
// String.fromCharCode is the built-in.
const kHexDigits = '0123456789ABCDEF';
const kByteEncodings: string[] = [];
const kUnencoded: boolean[] = [];
for (let byte = 0; byte < 256; byte++) {
  const unencoded =
    (byte >= 0x30 && byte <= 0x39) ||
    (byte >= 0x41 && byte <= 0x5a) ||
    (byte >= 0x61 && byte <= 0x7a) ||
    byte === 0x2a ||
    byte === 0x2d ||
    byte === 0x2e ||
    byte === 0x5f;
  kUnencoded[byte] = unencoded;
  if (unencoded) {
    kByteEncodings[byte] = String.fromCharCode(byte);
  } else if (byte === kSpace) {
    kByteEncodings[byte] = '+';
  } else {
    kByteEncodings[byte] =
      `%${kHexDigits[byte >> 4] as string}${kHexDigits[byte & 15] as string}`;
  }
}

function serializeComponent(input: string): string {
  const length = input.length;
  let i = 0;
  while (i < length) {
    const unit = StringPrototypeCharCodeAt(input, i);
    if (unit >= 128 || !kUnencoded[unit]) break;
    i++;
  }
  if (i === length) return input;
  let result = StringPrototypeSlice(input, 0, i);
  const bytes = TextEncoderEncode(kEncoder, StringPrototypeSlice(input, i));
  const byteLength = TypedArrayPrototypeGetLength(bytes);
  for (let b = 0; b < byteLength; b++) {
    result += kByteEncodings[bytes[b] as number] as string;
  }
  return result;
}

// https://url.spec.whatwg.org/#concept-urlencoded-serializer
function serializeUrlencoded(list: readonly SearchParamsEntry[]): string {
  let result = '';
  for (let i = 0; i < list.length; i++) {
    const entry = list[i] as SearchParamsEntry;
    if (i > 0) result += '&';
    result += `${serializeComponent(entry[0])}=${serializeComponent(entry[1])}`;
  }
  return result;
}

export type UrlencodedHelpers = {
  parseUrlencoded: typeof parseUrlencoded;
  serializeUrlencoded: typeof serializeUrlencoded;
};

module.exports = {
  parseUrlencoded,
  serializeUrlencoded,
};
